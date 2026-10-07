#include "two_core_rig.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/server/expeditor.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/recovery.hpp"
#include "kds/wal/redo.hpp"
#include "kds/wal/stream.hpp"

// **BC-S1: the premise WAL recycling rests on, tested before anything is
// built** (`instructions/v3.0.0/workorder-bc-wal-recycling.md` §1.4).
//
// Recovery starts its analysis at the anchor's redo start `D`, then
// recomputes the redo start from every `CHECKPOINT_BEGIN` in the scan
// (`analysis.cpp`), and nothing floors that at `D`. On two cores the
// recomputed start falls below `D`: a page an older checkpoint flushed and
// that stayed clean is still seeded at its old recLSN. Today redo reads
// those records and every one is skipped by `page_lsn`. Recycling removes
// them, so BC floors redo at `D` (BC-R2) - which is sound exactly when the
// anchor is, and these cells are what says so.
//
// Every cell recovers one crash image twice - from analysis's start, and
// from `max(start, D)` - and requires every page to come back byte-identical.
// A difference would mean a record below the anchor carried something the
// disk lacked: the anchor would be invalid, and BC would stop.

namespace kds::wal {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kP = server::kFirstUserPageId;
constexpr PageId kQ = server::kFirstUserPageId + 1;
constexpr std::uint64_t kTxn = 7;

// The floor BC-R2 builds: redo from the recomputed start, never below `D`.
AnalysisResult Floored(AnalysisResult a, Lsn d) {
    a.redo_start_lsn = std::max(a.redo_start_lsn, d);
    return a;
}

std::vector<std::byte> PageBytes(storage::PageStore& store, PageId id) {
    auto page = store.Get(id);
    EXPECT_TRUE(page.ok()) << page.status().message();
    if (!page.ok()) return {};
    return {page.value().bytes().begin(), page.value().bytes().end()};
}

void PutPage(storage::PageStore& store, PageId id, const std::vector<std::byte>& bytes) {
    auto existing = store.Get(id);
    if (existing.ok()) {
        std::memcpy(existing.value().bytes().data(), bytes.data(), bytes.size());
        return;
    }
    auto created = store.CreateAt(id);
    ASSERT_TRUE(created.ok()) << created.status().message();
    std::memcpy(created.value().bytes().data(), bytes.data(), bytes.size());
}

// The scripted log of §1.4: the records a two-core instance writes, with the
// writebacks it performs applied to a store standing in for the data file.
class ScriptedLog {
public:
    ScriptedLog() {
        device_ = std::move(MemoryLogDevice::Create(kSegmentSize).value());
        stream_ = std::move(WalStream::Open(device_.get(), 0).value());
    }

    MemoryLogDevice& device() { return *device_; }
    WalStream& stream() { return *stream_; }
    storage::InMemoryPageStore& disk() { return disk_; }

    Lsn Append(RecordType type, std::uint64_t txn, PageId page,
               std::span<const std::byte> payload = {}) {
        auto lsn = stream_->Append({type, txn, page}, payload);
        EXPECT_TRUE(lsn.ok()) << lsn.status().message();
        return lsn.ok() ? lsn.value() : 0;
    }

    Lsn PageInit(PageId page) {
        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
        EXPECT_TRUE(EncodePageInit(init, fields).ok());
        return Append(RecordType::kPageInit, kTxn, page, init);
    }

    Lsn Insert(PageId page, std::uint16_t slot, unsigned char fill) {
        const std::vector<std::byte> row(24, static_cast<std::byte>(fill));
        std::vector<std::byte> buf(kHeapWriteFixedSize + row.size(), std::byte{0});
        const HeapWritePayload hw{kTxn, /*undo_ptr=*/0, slot,
                                  static_cast<std::uint16_t>(row.size())};
        auto n = EncodeHeapWrite(buf, hw, row);
        EXPECT_TRUE(n.ok()) << n.status().message();
        return Append(RecordType::kHeapInsert, kTxn, page, std::span(buf).first(n.value()));
    }

    Lsn CheckpointBegin(std::vector<CheckpointDirtyPage> dirty) {
        std::vector<std::byte> payload(CheckpointBeginSize(0, dirty.size()), std::byte{0});
        auto n = EncodeCheckpointBegin(payload, {}, dirty);
        EXPECT_TRUE(n.ok()) << n.status().message();
        return Append(RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId,
                      std::span(payload).first(n.value()));
    }

    void CheckpointEnd(Lsn redo_start) {
        std::vector<std::byte> payload(kCheckpointEndPayloadSize, std::byte{0});
        auto n = EncodeCheckpointEnd(payload, CheckpointEndPayload{redo_start});
        EXPECT_TRUE(n.ok()) << n.status().message();
        Append(RecordType::kCheckpointEnd, kNoTxnId, kInvalidPageId,
               std::span(payload).first(n.value()));
    }

    // The page as the buffer pool holds it now: the whole log so far,
    // replayed onto nothing.
    std::vector<std::byte> Live(PageId page) {
        EXPECT_TRUE(stream_->Sync().ok());
        storage::InMemoryPageStore scratch{server::kFirstUserPageId};
        auto a = Analyze(*device_, 0, AnalysisStart{});
        EXPECT_TRUE(a.ok()) << a.status().message();
        auto r = Redo(*device_, 0, scratch, a.value());
        EXPECT_TRUE(r.ok()) << r.status().message();
        return PageBytes(scratch, page);
    }

    // A checkpoint's writeback of `page`: the live image reaches the disk.
    void Flush(PageId page) { PutPage(disk_, page, Live(page)); }

private:
    std::unique_ptr<MemoryLogDevice> device_;
    std::unique_ptr<WalStream> stream_;
    storage::InMemoryPageStore disk_{server::kFirstUserPageId};
};

// §1.4's sequence, one record at a time. Core A's first checkpoint flushes
// P, which then stays clean; Q's writeback races a write, so Q keeps its
// first recLSN (`wal.md` §11-2); core B's checkpoint records Q at that
// recLSN; core A's second has nothing dirty. The fold is B's - `q` - and A's
// first BEGIN, above it, still seeds P at `r < q`.
TEST(WalRecyclePremiseTest, RedoBelowTheAnchorRestoresNothingTheFlooredRedoDoesNot) {
    ScriptedLog log;
    log.Append(RecordType::kTxnBegin, kTxn, kInvalidPageId);
    const Lsn r = log.PageInit(kP);
    log.Insert(kP, 0, 0xA1);
    const Lsn q = log.PageInit(kQ);
    log.Insert(kQ, 0, 0xB1);

    // Core A's checkpoint #1: both pages dirty. P is written back; Q's copy
    // is raced by the write below and Q stays dirty at its first recLSN.
    const Lsn b1 = log.CheckpointBegin({{kP, r}, {kQ, q}});
    log.Flush(kP);
    log.Insert(kQ, 1, 0xB2);
    log.CheckpointEnd(r);

    // Core B's checkpoint: P is clean, Q is dirty at `q`. Its writeback of Q
    // is not raced.
    log.CheckpointBegin({{kQ, q}});
    log.Flush(kQ);
    log.CheckpointEnd(q);

    // Core A's checkpoint #2: nothing dirty, so its redo start is its BEGIN.
    const Lsn b3 = log.CheckpointBegin({});
    log.CheckpointEnd(b3);

    // The fold over each core's latest redo start.
    const Lsn d = std::min(b3, q);
    ASSERT_EQ(d, q);
    ASSERT_GE(b1, d) << "A's first BEGIN must lie inside the scan";
    ASSERT_LT(r, d);

    // Work after the last checkpoint, then the crash: P and Q dirty again,
    // neither written back.
    log.Insert(kP, 1, 0xA2);
    log.Insert(kQ, 2, 0xB3);
    log.Append(RecordType::kTxnCommit, kTxn, kInvalidPageId);
    ASSERT_TRUE(log.stream().Sync().ok());
    const std::vector<std::byte> want_p = log.Live(kP);
    const std::vector<std::byte> want_q = log.Live(kQ);

    auto a = Analyze(log.device(), 0, AnalysisStart{d, log.stream().durable_lsn()});
    ASSERT_TRUE(a.ok()) << a.status().message();
    // Not vacuous: the recomputed start is below the anchor, which is the
    // case the floor changes.
    ASSERT_LT(a.value().redo_start_lsn, d);
    EXPECT_EQ(a.value().redo_start_lsn, r);

    std::array<storage::InMemoryPageStore, 2> disks{
        storage::InMemoryPageStore{server::kFirstUserPageId},
        storage::InMemoryPageStore{server::kFirstUserPageId}};
    for (auto& disk : disks) {
        PutPage(disk, kP, PageBytes(log.disk(), kP));
        PutPage(disk, kQ, PageBytes(log.disk(), kQ));
    }
    auto unfloored = Redo(log.device(), 0, disks[0], a.value());
    ASSERT_TRUE(unfloored.ok()) << unfloored.status().message();
    auto floored = Redo(log.device(), 0, disks[1], Floored(a.value(), d));
    ASSERT_TRUE(floored.ok()) << floored.status().message();

    // The records below the anchor were read and changed nothing.
    EXPECT_GT(unfloored.value().records, floored.value().records);
    EXPECT_EQ(unfloored.value().applied, floored.value().applied);
    for (PageId page : {kP, kQ}) {
        EXPECT_EQ(PageBytes(disks[0], page), PageBytes(disks[1], page)) << "page " << page;
    }
    EXPECT_EQ(PageBytes(disks[1], kP), want_p);
    EXPECT_EQ(PageBytes(disks[1], kQ), want_q);
}

// §1.11's reading (`workorder-bc-wal-recycling.md`), outside BC's rules: a
// page a `CHECKPOINT_BEGIN` lists at recLSN 0 - dirtied by a path that logged
// nothing (`page_store_checkpoint_target.hpp`) - whose next record follows
// the BEGIN. Analysis seeds the page at 0 and `emplace` does not overwrite
// it with the record's LSN (`analysis.cpp`), so the page pulls nothing into
// the redo start. If nothing else does, redo starts past the record and the
// page on disk never receives it. The cell asserts the right answer: the
// record is replayed. **It is red at BC-S1**, and disabled until the defect
// is fixed: `docs/inflight/bugs/a-page-a-checkpoint-lists-at-reclsn-0-hides-its-next-record-from-redo.md`.
TEST(WalRecyclePremiseTest, DISABLED_APageSeededAtRecLsnZeroStillHasItsLaterRecordReplayed) {
    ScriptedLog log;
    log.Append(RecordType::kTxnBegin, kTxn, kInvalidPageId);
    const Lsn init = log.PageInit(kP);
    log.Insert(kP, 0, 0xC1);

    // A first checkpoint writes P back whole.
    log.CheckpointBegin({{kP, init}});
    log.Flush(kP);
    log.CheckpointEnd(init);

    // P is dirtied by something that logged nothing, so the next checkpoint
    // lists it at recLSN 0, and writes it back before P's next record.
    const Lsn b2 = log.CheckpointBegin({{kP, 0}});
    log.Flush(kP);
    const Lsn x = log.Insert(kP, 1, 0xC2);
    log.CheckpointEnd(b2);
    ASSERT_GT(x, b2);

    log.Append(RecordType::kTxnCommit, kTxn, kInvalidPageId);
    ASSERT_TRUE(log.stream().Sync().ok());
    const std::vector<std::byte> want = log.Live(kP);
    ASSERT_NE(PageBytes(log.disk(), kP), want) << "the disk must lack the record";

    auto a = Analyze(log.device(), 0, AnalysisStart{b2, log.stream().durable_lsn()});
    ASSERT_TRUE(a.ok()) << a.status().message();
    EXPECT_LE(a.value().redo_start_lsn, x)
        << "redo starts past P's record: the seeded 0 kept it out of the redo start";

    storage::InMemoryPageStore disk{server::kFirstUserPageId};
    PutPage(disk, kP, PageBytes(log.disk(), kP));
    auto redo = Redo(log.device(), 0, disk, a.value());
    ASSERT_TRUE(redo.ok()) << redo.status().message();
    EXPECT_EQ(PageBytes(disk, kP), want) << "P's committed record was not replayed";
}

// ---- The same comparison on two real cores ---------------------------------
//
// Both cores checkpoint into page 0's fold (`TwoCoreRig::Options::
// fold_anchor`), as an `Expeditor` at `cores = 2` does, and a crash image is
// recovered twice, unfloored and floored. **Only an image whose recomputed
// start is below the anchor tests anything**: elsewhere the floor is the
// start and the two runs are one. A randomized loop over threads never
// produced one in 112 images, because the anchor's publish syncs the whole
// store and writes back again the page a raced writeback left dirty - so
// the sequence is placed instead, through a writeback seam.

constexpr std::uint64_t kRigSegmentSize = 256 * 1024;

std::vector<char> FileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Ok(server::CommandDispatcher& d, const std::string& sql) {
    const std::string reply = d.Dispatch(sql).response;
    EXPECT_EQ(reply.rfind("ERR", 0), std::string::npos) << sql << " -> " << reply;
    return reply.rfind("ERR", 0) != 0;
}

std::string Update(int id, std::uint64_t value) {
    return "UPDATE t SET v = " + std::to_string(value) + " WHERE id = " + std::to_string(id);
}

// Redo over a copy of the image's data file; the recovered file's bytes.
StatusOr<std::vector<char>> RecoverCopy(const fs::path& image, const fs::path& copy,
                                        LogDevice& log, const AnalysisResult& a) {
    std::error_code ec;
    fs::copy_file(image / "kds.db", copy, fs::copy_options::overwrite_existing, ec);
    if (ec) return Status::IoError(ec.message());
    {
        auto device = storage::FilePageDevice::Open(copy.string());
        if (!device.ok()) return device.status();
        auto store = storage::DevicePageStore::Open(*device.value(), server::kFirstUserPageId);
        if (!store.ok()) return store.status();
        auto redo = Redo(log, 0, *store.value(), a);
        if (!redo.ok()) return redo.status();
        if (Status s = store.value()->Sync(); !s.ok()) return s;
    }
    return FileBytes(copy);
}

// Recovers one crash image twice, unfloored and floored, and requires the two
// data files to be identical. `below` says whether analysis's recomputed
// start fell below the anchor - the case the floor changes.
void ExpectFlooredRecoveryAgrees(const fs::path& image, const server::WalAnchorFields& anchor,
                                 bool& below) {
    auto log = FileLogDevice::Open((image / "wal").string(), 0, kRigSegmentSize);
    ASSERT_TRUE(log.ok()) << log.status().message();
    const Lsn d = anchor.redo_start_lsn;
    ASSERT_NE(d, 0u);
    auto a = Analyze(*log.value(), 0, AnalysisStart{d, anchor.durable_lsn});
    ASSERT_TRUE(a.ok()) << image << ": " << a.status().message();
    below = a.value().redo_start_lsn < d;

    auto unfloored = RecoverCopy(image, image / "unfloored.db", *log.value(), a.value());
    ASSERT_TRUE(unfloored.ok()) << image << ": " << unfloored.status().message();
    auto floored = RecoverCopy(image, image / "floored.db", *log.value(), Floored(a.value(), d));
    ASSERT_TRUE(floored.ok()) << image << ": " << floored.status().message();
    EXPECT_TRUE(unfloored.value() == floored.value())
        << image << ": redo from " << a.value().redo_start_lsn << " and from " << d
        << " recovered different data files";
}

// The page a single-row `INSERT` put its row on: `INSERTED ... page=N ...`.
PageId InsertedPage(const std::string& reply) {
    const std::size_t at = reply.find(" page=");
    EXPECT_NE(at, std::string::npos) << reply;
    return at == std::string::npos ? kInvalidPageId
                                   : static_cast<PageId>(std::stoull(reply.substr(at + 6)));
}

// §1.4's sequence on two real cores, placed rather than waited for. The
// write that races Q's writeback runs in the seam between Q's copy and its
// clean (`DevicePageStore::SetAfterWritebackCopyForTest`), so the sequence
// is the same on every run:
//
//   1. core 1 dirties P at `r`, core 0 dirties Q at `q > r`;
//   2. core 0's checkpoint A1 flushes P, which stays clean, and core 1's
//      write lands on Q between its copy and its clean: Q stays dirty at `q`;
//   3. core 1's checkpoint B1 sees Q at `q` and flushes it;
//   4. core 0's checkpoint A2 has nothing older, so the fold is B1's - at or
//      below `q` - and A1's BEGIN, above it, still seeds P at `r`.
//
// `padding_rows` rows into a second relation come first and again between
// P's write and Q's, so the log is several segments long and `r` and `q`
// lie in different segments. The caller takes the fold and the crash image.
void RunPlacedSequence(server::TwoCoreRig& rig, int padding_rows) {
    server::CommandDispatcher& d0 = rig.core(0).dispatcher();
    server::CommandDispatcher& d1 = rig.core(1).dispatcher();
    int next_pad = 1;
    const auto pad = [&] {
        CurrentCoreGuard as(0);
        for (const int end = next_pad + padding_rows; next_pad < end; next_pad += 100) {
            std::string sql = "INSERT INTO pad VALUES ";
            for (int k = next_pad; k < next_pad + 100; ++k) {
                sql += (k == next_pad ? "" : ", ") + std::string("(") + std::to_string(k) + ", 0)";
            }
            ASSERT_TRUE(Ok(d0, sql));
        }
    };

    // Two leaves at least: rows 1..198 fill the first leaf, so row 10 and
    // row 300 are on different pages.
    PageId p = kInvalidPageId;
    PageId q = kInvalidPageId;
    {
        CurrentCoreGuard as(0);
        ASSERT_TRUE(Ok(d0, "CREATE TABLE t (id int64, v int64) BTREE"));
        ASSERT_TRUE(Ok(d0, "CREATE TABLE pad (id int64, v int64) BTREE"));
    }
    pad();
    {
        CurrentCoreGuard as(0);
        for (int id = 1; id <= 400; ++id) {
            const std::string reply =
                d0.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", 0)").response;
            ASSERT_EQ(reply.rfind("ERR", 0), std::string::npos) << reply;
            if (id == 10) p = InsertedPage(reply);
            if (id == 300) q = InsertedPage(reply);
        }
    }
    ASSERT_NE(p, q);
    {
        // Core 1's transaction-id window, carved now: a carve syncs the
        // store, which the seam's write must not have to do.
        CurrentCoreGuard as(1);
        ASSERT_TRUE(Ok(d1, Update(400, 1)));
    }
    ASSERT_TRUE(rig.core(0).Checkpoint().ok());
    ASSERT_TRUE(rig.core(1).Checkpoint().ok());

    // 1.
    {
        CurrentCoreGuard as(1);
        ASSERT_TRUE(Ok(d1, Update(10, 2)));
    }
    pad();
    {
        CurrentCoreGuard as(0);
        ASSERT_TRUE(Ok(d0, Update(300, 2)));
    }

    // 2. The seam fires on core 0's checkpoint thread, which here is this
    // one; the write in it is core 1's.
    //
    // **Every copy of Q the checkpoint makes is raced, not only the first.**
    // The anchor's publish syncs the whole store
    // (`superblock_checkpoint_anchor.cpp`), which writes Q back a second
    // time; a write that raced only the checkpoint's own pass is carried out
    // by that sync and Q comes out clean.
    int raced = 0;
    std::uint64_t value = 3;
    rig.store().SetAfterWritebackCopyForTest([&](PageId page) {
        if (page != q) return;
        ++raced;
        CurrentCoreGuard as(1);
        EXPECT_TRUE(Ok(d1, Update(301, ++value)));
    });
    ASSERT_TRUE(rig.core(0).Checkpoint().ok());
    rig.store().SetAfterWritebackCopyForTest(nullptr);
    ASSERT_GT(raced, 0);
    bool q_still_dirty = false;
    for (const auto& [page, rec_lsn] : rig.store().DirtyPagesWithRecLsn()) {
        if (page == q) q_still_dirty = rec_lsn != 0;
    }
    ASSERT_TRUE(q_still_dirty) << "the raced write must keep Q dirty at its first recLSN";

    // 3 and 4.
    ASSERT_TRUE(rig.core(1).Checkpoint().ok());
    ASSERT_TRUE(rig.core(0).Checkpoint().ok());

    // Work after the last checkpoint, then the crash.
    {
        CurrentCoreGuard as(0);
        ASSERT_TRUE(Ok(d0, Update(11, 4)));
        ASSERT_TRUE(Ok(d0, Update(302, 4)));
    }
}

server::TwoCoreRig::Options PlacedRigOptions(bool recycle) {
    server::TwoCoreRig::Options options;
    options.file_backed = true;
    options.fold_anchor = true;
    options.recycle = recycle;
    options.wal_segment_bytes = kRigSegmentSize;
    return options;
}

TEST(WalRecyclePremiseRigTest, ARaceInsideAWritebackPutsRedoBelowTheFoldAndTheFloorAgrees) {
    const fs::path image = fs::temp_directory_path() /
                           ("kds_bc_s1_placed_" + std::to_string(::getpid()));
    // A failed run leaves its image; a reused pid would snapshot over it,
    // and `Snapshot` keeps segment files the new log does not have.
    fs::remove_all(image);
    server::WalAnchorFields anchor{};
    {
        auto opened = server::TwoCoreRig::Open(PlacedRigOptions(/*recycle=*/false));
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        RunPlacedSequence(*opened.value(), /*padding_rows=*/0);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        anchor = opened.value()->folded_anchor();
        ASSERT_TRUE(opened.value()->Snapshot(image).ok());
    }

    bool below = false;
    ExpectFlooredRecoveryAgrees(image, anchor, below);
    // Not vacuous: this image is the case the floor changes.
    EXPECT_TRUE(below) << "the placed sequence must put analysis's start below the fold";

    std::error_code ec;
    fs::remove_all(image, ec);
}

// **BC-S3: §1.4's sequence with recycling on, then the mount.** The log is
// several segments long, the fold's durable advances have removed every
// segment wholly below it, and analysis still recomputes a start below the
// fold. Recovery opens the log from the anchor's segment and succeeds only
// because redo is floored there (BC-R2); without the floor its scan would
// start in a segment that is gone.
TEST(WalRecyclePremiseRigTest, AfterRecyclingTheMountRecoversBecauseRedoIsFloored) {
    const fs::path image = fs::temp_directory_path() /
                           ("kds_bc_s3_recycled_" + std::to_string(::getpid()));
    fs::remove_all(image);
    server::WalAnchorFields anchor{};
    {
        auto opened = server::TwoCoreRig::Open(PlacedRigOptions(/*recycle=*/true));
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        server::TwoCoreRig& rig = *opened.value();
        // The removal is the writer thread's; wait for it before the crash
        // image, so the image holds no segment the run detached.
        const auto reclaimed = [&] {
            return rig.log_device().segments_removed() == rig.log_device().first_segment();
        };
        RunPlacedSequence(rig, /*padding_rows=*/1500);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_TRUE(server::Within(std::chrono::seconds(5), reclaimed));
        ASSERT_GT(rig.log_device().first_segment(), 0u) << "the run must have recycled";
        anchor = rig.folded_anchor();
        ASSERT_TRUE(rig.Snapshot(image).ok());
    }
    ASSERT_GT(anchor.segment_no, 0u);
    for (std::uint64_t s = 0; s < anchor.segment_no; ++s) {
        ASSERT_FALSE(fs::exists(image / "wal" / ("wal-0-" + std::to_string(s) + ".log")))
            << "segment " << s << " is below the anchor and was recycled";
    }

    auto log = FileLogDevice::Open((image / "wal").string(), 0, kRigSegmentSize,
                                   /*first_needed=*/anchor.segment_no);
    ASSERT_TRUE(log.ok()) << log.status().message();
    const AnalysisStart start{anchor.redo_start_lsn, anchor.durable_lsn};

    // Without the floor: the recomputed start is in a recycled segment.
    auto a = Analyze(*log.value(), 0, start);
    ASSERT_TRUE(a.ok()) << a.status().message();
    ASSERT_LT(a.value().redo_start_lsn / kRigSegmentSize, anchor.segment_no)
        << "not vacuous: analysis must reach into a recycled segment";
    {
        auto device = storage::FilePageDevice::Open((image / "kds.db").string());
        ASSERT_TRUE(device.ok());
        auto store = storage::DevicePageStore::Open(*device.value(), server::kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        auto unfloored = Redo(*log.value(), 0, *store.value(), a.value());
        ASSERT_FALSE(unfloored.ok());
        EXPECT_EQ(unfloored.status().code(), StatusCode::kCorruption)
            << unfloored.status().message();
    }

    // With it: recovery succeeds, and reports how far below it was asked to go.
    std::error_code ec;
    fs::copy_file(image / "kds.db", image / "mounted.db", fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);
    auto device = storage::FilePageDevice::Open((image / "mounted.db").string());
    ASSERT_TRUE(device.ok());
    auto store = storage::DevicePageStore::Open(*device.value(), server::kFirstUserPageId);
    ASSERT_TRUE(store.ok());
    auto report = RecoverCore(*log.value(), 0, *store.value(), start);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().analysis.redo_start_lsn, anchor.redo_start_lsn);
    EXPECT_EQ(report.value().redo_start_floored_from, a.value().redo_start_lsn);

    fs::remove_all(image, ec);
}

// §1.11 through production's mount: a crash, a mount whose redo dirties pages
// it logs nothing for, a committed `INSERT` onto those pages, and a second
// crash before any checkpoint after the mount's own.
StatusOr<std::unique_ptr<server::Expeditor>> MountImage(const fs::path& image) {
    server::Expeditor::Config config;
    config.data_file = (image / "kds.db").string();
    config.wal_dir = (image / "wal").string();
    config.log_file = {};
    config.cores = 2;
    config.debug_text_port = 0;
    return server::Expeditor::Open(config, /*now_unix_seconds=*/2000);
}

// What a crash of a mounted, unstarted `Expeditor` leaves: its files as they
// are, the pool's unwritten pages lost with it. Taken before the instance is
// destroyed, because destruction checkpoints.
void CopyImage(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to);
    fs::copy_file(from / "kds.db", to / "kds.db", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(from / "wal", to / "wal", fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << ec.message();
}

TEST(WalRecyclePremiseRigTest, ARowCommittedAfterAMountSurvivesTheNextCrash) {
    const fs::path root = fs::temp_directory_path() /
                          ("kds_bc_s1_remount_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    {
        auto opened = server::TwoCoreRig::Open([] {
            server::TwoCoreRig::Options options;
            options.file_backed = true;
            return options;
        }());
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        server::TwoCoreRig& rig = *opened.value();
        CurrentCoreGuard as(0);
        ASSERT_TRUE(Ok(rig.core(0).dispatcher(), "CREATE TABLE t (id int64, v int64) BTREE"));
        ASSERT_TRUE(Ok(rig.core(0).dispatcher(), "CREATE INDEX tv ON t (v)"));
        ASSERT_TRUE(Ok(rig.core(0).dispatcher(), "INSERT INTO t VALUES (1, 1)"));
        // The first crash: nothing written back since the create, so the
        // mount below redoes the leaf and the catalog pages.
        ASSERT_TRUE(rig.Snapshot(root / "first").ok());
    }
    {
        auto mounted = MountImage(root / "first");
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        ASSERT_TRUE(Ok(mounted.value()->dispatcher(), "INSERT INTO t VALUES (2, 2)"));
        // The second crash, before any checkpoint but the mount's own.
        CopyImage(root / "first", root / "second");
    }
    {
        auto mounted = MountImage(root / "second");
        ASSERT_TRUE(mounted.ok()) << mounted.status().message();
        server::CommandDispatcher& d = mounted.value()->dispatcher();
        // A guard, green at BC-S1. The first mount's completion checkpoint
        // lists every page it redid at recLSN 0, so the second mount's redo
        // skips the insert's `sys.tables` bump (`bugs/a-page-a-checkpoint-
        // lists-at-reclsn-0-hides-its-next-record-from-redo.md`). What a
        // client sees survives it: the row is on a page a later record
        // brings into the redo start, and the high-water repair restores
        // the mark from the row.
        const std::string by_index = d.Dispatch("SELECT id FROM t WHERE v = 2").response;
        EXPECT_NE(by_index.find("\\n2"), std::string::npos)
            << "the row committed after the first mount is missing through its index: "
            << by_index;
        const std::string issued = d.Dispatch("INSERT INTO t VALUES (9)").response;
        EXPECT_EQ(issued.rfind("ERR", 0), std::string::npos) << issued;
        const std::string ids = d.Dispatch("SELECT id FROM t").response;
        EXPECT_EQ(ids.find("\\n2\\n2"), std::string::npos) << "an id was issued twice: " << ids;
        EXPECT_NE(ids.find("\\n3"), std::string::npos) << "the issued id is not 3: " << ids;
    }
    fs::remove_all(root, ec);
}

}  // namespace
}  // namespace kds::wal
