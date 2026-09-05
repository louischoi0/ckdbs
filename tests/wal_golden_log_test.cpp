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
// (`v2.7.0-134-gd15b5ac`), before AL-S1a touched the stream.
constexpr std::uint32_t kGoldenLogCrc = 0x07b052c3u;

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
    all.reserve(static_cast<std::size_t>(segment_size * device.segment_count()));
    for (std::uint64_t no = 0; no < device.segment_count(); ++no) {
        EXPECT_TRUE(device.ReadAt(no, 0, segment).ok());
        all.insert(all.end(), segment.begin(), segment.end());
    }
    return storage::Crc32c(all);
}

std::uint32_t RunScript() {
    auto created = SimInstance::Create();
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
