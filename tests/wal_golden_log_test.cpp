#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/crc32c.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "sim/instance.hpp"

// AL-R7's golden log (`instructions/v3.0.0/workorder-al-m0-single-wal.md`):
// the `cores = 1` WAL bytes are pinned, so that every stage of the single
// stream can prove it changed nothing on the path a single-core instance
// runs. One fixed statement script over a fresh in-memory instance, a clean
// shutdown, and a CRC32C over every segment byte the log device holds.
//
// The pin is a *contract on the bytes*, so it moves only when the bytes are
// meant to move - a record format change, a new record on this path - and
// the commit that moves it says why. A mismatch with no such commit is the
// regression this test exists to catch.
//
// Two assertions, deliberately distinct: two fresh instances running the
// script agree with each other (the engine is deterministic on this path),
// and both agree with the pin (the bytes are the pinned bytes). A failure
// of the first is nondeterminism, never a format drift, and must not be
// answered by re-pinning.

namespace kds::sim {
namespace {

// Pinned on `worktree-v3.0.0-arch-revision` from the engine at `d15b5ac`
// (`v2.7.0-134-gd15b5ac`), before AL-S1a touched the stream, as
// 0x07b052c3. **Re-pinned at AT-S21** (on `at-s21-log-under-hold`), and for
// one reason: a spill is noted and logged at its append, under its var-heap
// page's hold - its UNDO_WRITE then its VARHEAP_APPEND - where it was noted
// after the row's placement and logged inside `LogInsert`. Row 6 is the
// script's one spill (the updates spill nothing and no index exists), so its
// two spill records move ahead of the row's own undo record and `HEAP_INSERT`;
// RV3's undo-before-append order holds, and no record's bytes change.
// **Re-pinned when the split relation was retired** (on
// `retire-split-relations`, `raft-marks-2026-09-30.md` §9): bootstrap no
// longer creates `sys.ranges`, so its `sys.objects` and `sys.tables` rows -
// both logged through `InsertRow` - leave the stream, and every later
// record's LSN moves with them.
// **Re-pinned at BB-S3** (on `bb-issue-under-the-leaf`,
// `instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md` BB-R3): a named
// key's encode now precedes its admission, so its `sys.tables` mark record
// follows its spill records where it used to precede them. Row 6 is the
// script's one spill - a heap row, admitted just before `ChainInsert` as the
// heap arm was at BB-S3 - so its UNDO_WRITE and VARHEAP_APPEND move ahead of
// its mark record; no record's bytes change, and every other row's records
// keep their order.
// **Re-pinned at BD-S2** (on `keep-btree-leaf-slots`,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` BD-R3 E2): a
// row placed in a B+ tree leaf logs `BTREE_INSERT` (type 29), where it
// logged `HEAP_INSERT` (type 5). `golden_tree`'s two rows are the script's
// two such records; their payloads are byte-identical, so the stream moves
// by the two type bytes and their CRCs, and every heap record is unchanged.
constexpr std::uint32_t kGoldenLogCrc = 0xd4d0579au;

const char* const kScript[] = {
    "CREATE TABLE golden_heap (id int64, v int64, name varchar) HEAP",
    "CREATE TABLE golden_tree (id int64, v int64, name varchar) BTREE",
    "INSERT INTO golden_heap VALUES (1, 10, 'one')",
    "INSERT INTO golden_heap VALUES (2, 20, 'two')",
    "INSERT INTO golden_heap VALUES (3, 30, 'three')",
    "INSERT INTO golden_tree VALUES (1, 100, 'hundred')",
    "INSERT INTO golden_tree VALUES (2, 200, 'two hundred')",
    "UPDATE golden_heap SET v = 21 WHERE id = 2",
    "DELETE FROM golden_heap WHERE id = 3",
    "BEGIN",
    "INSERT INTO golden_heap VALUES (4, 40, 'four')",
    "UPDATE golden_tree SET v = 201 WHERE id = 2",
    "COMMIT",
    "BEGIN",
    "INSERT INTO golden_heap VALUES (5, 50, 'five')",
    "ROLLBACK",
    // A value past the inline width, so the var-heap logs too.
    "INSERT INTO golden_heap VALUES (6, 60, '"
    "spilled-spilled-spilled-spilled-spilled-spilled-spilled-spilled-"
    "spilled-spilled-spilled-spilled-spilled-spilled-spilled-spilled')",
};

// The whole log, segment by segment, as the device holds it after the
// clean shutdown - including the segment headers and the zeroed tails, so
// that a record moving by one byte moves the answer.
std::uint32_t LogCrc(wal::MemoryLogDevice& device) {
    const std::uint64_t segment_size = device.segment_size();
    std::vector<std::byte> segment(static_cast<std::size_t>(segment_size));
    std::vector<std::byte> all;
    all.reserve(static_cast<std::size_t>(segment_size * device.end_segment()));
    for (std::uint64_t no = 0; no < device.end_segment(); ++no) {
        EXPECT_TRUE(device.ReadAt(no, 0, segment).ok());
        all.insert(all.end(), segment.begin(), segment.end());
    }
    return storage::Crc32c(all);
}

std::uint32_t RunScript() {
    // Its own segment size, pinned with the bytes: the golden is a statement
    // about the record format, and the simulator's default segment moved to
    // 64 KiB for recycling (BC-S4) without the format moving at all.
    SimInstanceOptions options;
    options.wal_segment_bytes = 1ull << 20;
    auto created = SimInstance::Create(options);
    EXPECT_TRUE(created.ok()) << created.status().message();
    if (!created.ok()) return 0;
    SimInstance& db = *created.value();
    for (const char* sql : kScript) {
        const std::string reply = db.Execute(sql);
        EXPECT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    EXPECT_TRUE(db.CleanShutdown().ok());
    return LogCrc(db.log_device());
}

TEST(WalGoldenLog, TheSingleCoreScriptIsDeterministic) {
    EXPECT_EQ(RunScript(), RunScript());
}

TEST(WalGoldenLog, TheSingleCoreScriptWritesThePinnedBytes) {
    const std::uint32_t crc = RunScript();
    EXPECT_EQ(crc, kGoldenLogCrc) << "actual crc32c 0x" << std::hex << crc
                                  << " - if the bytes were meant to move, re-pin "
                                     "in the same commit and say why";
}

}  // namespace
}  // namespace kds::sim
