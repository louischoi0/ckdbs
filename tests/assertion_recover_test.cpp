#include "kds/exec/assertion_recover.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"

// Assertion recovery end to end (AS6a, workplan RC07 part 3): a checkpoint's
// group-header snapshot plus the records after it, folded back into an enforcing
// directory.
//
// The tests are written against the boundary the snapshot *introduces*, because
// that is the part no earlier task could have covered:
//
//   - a group whose entries **span** the checkpoint has to re-sum to the same
//     total, with the pre-checkpoint half arriving from the snapshot and the
//     post-checkpoint half from the fold. Double-counting the first half is the
//     failure this design's whole shape is chosen to prevent;
//   - an assertion whose records appear with **no snapshot** must not be folded
//     onto nothing, because the aggregates would be too small and an admission
//     check built on them admits a write that violates the assertion;
//   - the linkage the snapshot deliberately does not carry has to come back from
//     the cabin's own pages, which is what `group_id` on the entry is for.

namespace kds::exec {
namespace {

using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryBytes;
using storage::cabin::kEntryHintValid;

constexpr std::uint64_t kSegmentSize = 64 * 1024;
constexpr std::uint64_t kAssertionId = 77;
constexpr PageId kCabinPage = 300;

std::string Key(std::string s) {
    std::vector<parser::AstValue> values;
    parser::AstValue v;
    v.type = parser::ValueType::kStr;
    v.str_val = std::move(s);
    values.push_back(std::move(v));
    return EncodeGroupKey(values);
}

class AssertionRecoverTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        auto manager = wal::WalManager::Open(device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        wal_ = std::move(manager.value());

        // The cabin's one page, formatted the way its birth would.
        auto page = store_.CreateAt(kCabinPage);
        ASSERT_TRUE(page.ok()) << page.status().message();
        ASSERT_TRUE(BoundCabinPage::Format(page.value().bytes()).ok());
    }

    // Writes one entry into the cabin page and applies it to `live`, exactly as
    // the CREATE-time builder and the enforcer do: id first, then the page, then
    // the directory.
    std::uint16_t Write(BoundCabin& live, const std::string& key, std::int64_t value,
                        std::uint64_t pk) {
        BoundCabinEntry entry;
        entry.pk = pk;
        entry.flags = kEntryHintValid;
        entry.value = value;
        entry.group_id = live.EnsureGroupId(key);

        auto page = store_.Get(kCabinPage);
        EXPECT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        EXPECT_TRUE(view.ok());
        auto index = view.value().Append(entry);
        EXPECT_TRUE(index.ok()) << index.status().message();
        EXPECT_TRUE(live.Apply(key, value, kCabinPage, index.value()).ok());
        return index.value();
    }

    // The live abort's page half: the directory drops the linkage (the caller's
    // own `Unapply`) and the page keeps the bytes with `kEntryOrphaned` set,
    // exactly as `AssertionEnforcer::AbortTxn` does it.
    void MarkOrphaned(std::uint16_t index) {
        auto page = store_.Get(kCabinPage);
        ASSERT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        ASSERT_TRUE(view.ok());
        ASSERT_TRUE(view.value().MarkOrphaned(index).ok());
    }

    // ASSERT_BUILD for an entry already on the page, so the fold has a record to
    // apply for the post-checkpoint half.
    void LogEntry(std::uint16_t index, const std::string& key, std::uint32_t group_id) {
        auto page = store_.Get(kCabinPage);
        ASSERT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        ASSERT_TRUE(view.ok());
        auto entry = view.value().Read(index);
        ASSERT_TRUE(entry.ok());

        std::array<std::byte, kEntryBytes> bytes{};
        ASSERT_TRUE(storage::cabin::EncodeEntry(entry.value(), bytes).ok());
        std::vector<std::byte> payload(wal::kAssertEntryFixedSize + kEntryBytes + key.size());
        wal::AssertEntryPayload fields{};
        fields.assertion_id = kAssertionId;
        fields.index = index;
        fields.group_id = group_id;
        auto used = wal::EncodeAssertEntry(
            payload, fields, bytes,
            std::as_bytes(std::span<const char>(key.data(), key.size())));
        ASSERT_TRUE(used.ok()) << used.status().message();
        ASSERT_TRUE(wal_->Append({wal::RecordType::kAssertBuild, wal::kNoTxnId, kCabinPage},
                                 std::span(payload).first(used.value()))
                        .ok());
    }

    // ASSERT_ROLLBACK for an entry the live side has already un-applied and
    // marked, so the fold has the abort's record to compensate from.
    void LogRollback(std::uint16_t index, const std::string& key, std::int64_t delta) {
        std::vector<std::byte> payload(wal::kAssertRollbackFixedSize + key.size());
        wal::AssertRollbackPayload fields{};
        fields.assertion_id = kAssertionId;
        fields.delta = delta;
        fields.index = index;
        auto used = wal::EncodeAssertRollback(
            payload, fields, std::as_bytes(std::span<const char>(key.data(), key.size())));
        ASSERT_TRUE(used.ok()) << used.status().message();
        ASSERT_TRUE(wal_->Append({wal::RecordType::kAssertRollback, /*txn_id=*/9, kCabinPage},
                                 std::span(payload).first(used.value()))
                        .ok());
    }

    // The checkpoint, through the real Checkpointer and the real seam - so the
    // snapshot under test is the one a mount would actually find.
    wal::Lsn Checkpoint(const BoundCabin& live) {
        class Source final : public wal::AssertionSnapshotSource {
        public:
            explicit Source(const BoundCabin& cabin) : cabin_(cabin) {}
            Status VisitSnapshots(std::size_t, const wal::SnapshotVisitor& visit) override {
                // The seam owns its keys, so this is a straight copy - the
                // first version of this fixture had to keep a `mutable` vector
                // of strings alive across the call, which is the trap that
                // moved the seam to owned keys.
                wal::AssertionCabinSnapshot out;
                out.assertion_id = kAssertionId;
                for (const BoundCabin::GroupSnapshot& g : cabin_.SnapshotGroups()) {
                    wal::AssertionSnapshotGroup entry;
                    entry.group_id = g.group_id;
                    entry.count = g.count;
                    entry.sum = g.sum;
                    entry.key = g.key;
                    out.groups.push_back(std::move(entry));
                }
                return visit({out});
            }

        private:
            const BoundCabin& cabin_;
        };

        class NoPages final : public wal::CheckpointTarget {
        public:
            std::vector<wal::CheckpointDirtyPage> DirtyTable() const override { return {}; }
            Status FlushPages(std::span<const PageId>) override { return Status::OK(); }
        };

        Source source(live);
        NoPages target;
        wal::NoActiveTransactions none;
        wal::InMemoryCheckpointAnchor anchor;
        wal::Checkpointer checkpointer(*wal_, target, none, anchor);
        checkpointer.SetAssertionSource(&source);
        EXPECT_TRUE(checkpointer.RunToCompletion().ok());
        EXPECT_EQ(anchor.publishes(), 1u);
        return anchor.anchor().checkpoint_lsn;
    }

    // How many ASSERT_SNAPSHOT records the stream holds from `from_lsn`. A
    // chunking test that did not reach the boundary would pass vacuously, and
    // the arithmetic that decides how many chunks a cabin needs is not something
    // a reader of the test can check by eye.
    std::uint64_t SnapshotRecords(wal::Lsn from_lsn) {
        EXPECT_TRUE(wal_->Flush().ok());
        std::uint64_t n = 0;
        auto scanned = wal::ScanLog(*device_, /*core_id=*/0, from_lsn,
                                    [&n](const wal::DecodedRecord& record) {
                                        if (record.type() == wal::RecordType::kAssertSnapshot) ++n;
                                        return Status::OK();
                                    });
        EXPECT_TRUE(scanned.ok()) << scanned.status().message();
        return n;
    }

    StatusOr<AssertionRecoveryReport> Recover(wal::Lsn from_lsn, BoundCabin& into) {
        RecoverableAssertion a;
        a.assertion_id = kAssertionId;
        a.root_page_id = kCabinPage;
        a.cabin = &into;
        const std::array<RecoverableAssertion, 1> list = {a};
        // Durable before it is read back: the manager stages appends, and a
        // scan reads the device (log_scanner.hpp), so a test that skipped this
        // would be reading a stream the writer has not published.
        EXPECT_TRUE(wal_->Flush().ok());
        return RecoverAssertions(*device_, /*core_id=*/0, from_lsn, store_, list, /*log=*/nullptr);
    }

    // `live`'s snapshot run as the writer emits it, one payload per chunk -
    // logged into a scratch stream of its own and read back, so a cell can
    // replay the run into `wal_` with something between its chunks, or with
    // chunks missing. The chunks are the real writer's bytes, never
    // hand-encoded ones: what is under test is how recovery reads what
    // `LogAssertionSnapshot` writes.
    std::vector<std::vector<std::byte>> SnapshotChunks(const BoundCabin& live) {
        std::vector<std::vector<std::byte>> out;
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        EXPECT_TRUE(device.ok());
        if (!device.ok()) return out;
        auto manager = wal::WalManager::Open(device.value().get(), clock_, /*core_id=*/0);
        EXPECT_TRUE(manager.ok());
        if (!manager.ok()) return out;

        wal::AssertionCabinSnapshot cabin;
        cabin.assertion_id = kAssertionId;
        for (const BoundCabin::GroupSnapshot& g : live.SnapshotGroups()) {
            cabin.groups.push_back(wal::AssertionSnapshotGroup{g.group_id, g.count, g.sum, g.key});
        }
        EXPECT_TRUE(wal::LogAssertionSnapshot(*manager.value(), cabin).ok());
        EXPECT_TRUE(manager.value()->Flush().ok());
        auto scanned = wal::ScanLog(*device.value(), /*core_id=*/0, /*from_lsn=*/0,
                                    [&out](const wal::DecodedRecord& record) {
                                        if (record.type() == wal::RecordType::kAssertSnapshot) {
                                            out.emplace_back(record.payload.begin(),
                                                             record.payload.end());
                                        }
                                        return Status::OK();
                                    });
        EXPECT_TRUE(scanned.ok()) << scanned.status().message();
        return out;
    }

    wal::Lsn AppendChunk(const std::vector<std::byte>& payload) {
        auto lsn = wal_->Append({wal::RecordType::kAssertSnapshot, wal::kNoTxnId, kInvalidPageId},
                                payload);
        EXPECT_TRUE(lsn.ok()) << lsn.status().message();
        return lsn.ok() ? lsn.value() : 0;
    }

    // Another core's record: under one stream nothing keeps it out of the gap
    // between two chunks (the directory latch keeps out only ASSERT_*).
    void AppendForeignRecord() {
        ASSERT_TRUE(
            wal_->Append({wal::RecordType::kTxnCommit, /*txn_id=*/41, kInvalidPageId}).ok());
    }

    // A cabin whose snapshot takes several records - 700 and not 600, which
    // fell 302 bytes short of chunking (`ManyGroupsChunkAcrossRecords...`).
    static void FillToChunk(BoundCabin& live) {
        for (int i = 0; i < 700; ++i) {
            live.EnsureGroupId(Key("group-" + std::string(60, 'k') + std::to_string(i)));
        }
    }

    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> device_;
    std::unique_ptr<wal::WalManager> wal_;
    storage::InMemoryPageStore store_{200};
};

TEST_F(AssertionRecoverTest, AGroupWhoseEntriesSpanTheCheckpointReSumsCorrectly) {
    // The boundary the snapshot introduces, and the workplan's named extra test.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);

    // Before the checkpoint: two entries whose contribution the snapshot carries.
    Write(live, Key("x"), 5, /*pk=*/1);
    Write(live, Key("x"), 7, /*pk=*/2);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    // After it: a third entry in the same group, whose contribution has to come
    // from the fold instead.
    const std::uint32_t gx = live.Find(Key("x"))->group_id;
    const std::uint16_t after = Write(live, Key("x"), 11, /*pk=*/3);
    LogEntry(after, Key("x"), gx);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    ASSERT_EQ(report.value().assertions.size(), 1u);
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(report.value().records_without_a_base, 0u);

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->group_id, gx) << "the rebuilt group must keep the id its entries carry";
    // 5 + 7 from the snapshot, 11 from the fold. Double-counting the first two
    // would read 24, folding without the base 11 - both are the failures this
    // design exists to avoid, and both are one arithmetic slip away.
    EXPECT_EQ(x->sum, 23);
    EXPECT_EQ(x->count, 3);
    EXPECT_EQ(x->sum, live.Find(Key("x"))->sum) << "the rebuild disagrees with the live directory";
    EXPECT_EQ(x->count, live.Find(Key("x"))->count);
}

TEST_F(AssertionRecoverTest, TheLinkageComesBackFromTheCabinsOwnPages) {
    // AS6a's reason for putting `group_id` on the entry: the snapshot is headers
    // only, so the entry list has to be rebuilt by reading the pages.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    Write(live, Key("x"), 5, 1);
    Write(live, Key("y"), 9, 2);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().assertions[0].groups_restored, 2u);
    EXPECT_EQ(report.value().assertions[0].entries_attached, 2u)
        << "the snapshot carries no entry lists, so these can only have come from the pages";

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    ASSERT_EQ(x->entries.size(), 1u);
    EXPECT_EQ(x->entries[0].first, kCabinPage);

    // And §7's verification hook over the rebuilt structure, reading the real
    // pages: header == Σ(entries), a rebuild checked against durable bytes.
    auto read = [this](PageId page_id, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        auto page = store_.Get(page_id);
        if (!page.ok()) return page.status();
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        return view.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok());
}

TEST_F(AssertionRecoverTest, RecordsWithNoSnapshotAreCountedAndTheAssertionStaysUnrecovered) {
    // Records but no base: folding them would produce aggregates that are too
    // small, and an admission check on those admits a violating write. So the
    // assertion is reported unrecovered - `enforcing=0` - rather than enforcing
    // wrongly. **A BUILD record included** - the deleted genesis rule's
    // record of why lives at the skip itself (`assertion_recover.cpp`); the
    // born-after-checkpoint case this used to tempt is the publish-time
    // snapshot's, not this arm's.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    const std::uint16_t index = Write(live, Key("x"), 5, 1);
    LogEntry(index, Key("x"), live.Find(Key("x"))->group_id);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(/*from_lsn=*/0, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_FALSE(report.value().assertions[0].recovered);
    EXPECT_EQ(report.value().records_without_a_base, 1u);
    EXPECT_EQ(rebuilt.group_count(), 0u) << "nothing may be folded onto a base that does not exist";
}

TEST_F(AssertionRecoverTest, AnEmptyCabinStillGetsASnapshotSoItsAbsenceMeansSomething) {
    // A cabin with no groups is recovered *and* empty, which must not read the
    // same as a cabin whose snapshot is missing - the first can enforce, the
    // second cannot.
    BoundCabin live(BoundAggregate::kCount, /*bound=*/10);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    BoundCabin rebuilt(BoundAggregate::kCount, /*bound=*/10);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(report.value().assertions[0].groups_restored, 0u);
    EXPECT_EQ(rebuilt.group_count(), 0u);
}

TEST_F(AssertionRecoverTest, ManyGroupsChunkAcrossRecordsAndAllOfThemComeBack) {
    // A cabin's group count is bounded by the data, so the snapshot chunks, and
    // the loader takes the run as a base once every chunk it counts has arrived.
    //
    // Groups only, no entries: these headers exceed what one record's payload can
    // hold, which is the boundary under test. Entries would add nothing to it and
    // would need a chain of pages (one holds 254), so the linkage rebuild is left
    // to the test above that is about it.
    //
    // **700 and not 600.** At 600 the payload came to 61,106 bytes against a
    // 61,408-byte budget - 302 bytes short of chunking, so the test asserted the
    // boundary without reaching it. The record count below is what keeps that
    // from happening silently again.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const std::size_t groups = live.group_count();
    ASSERT_EQ(groups, 700u);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);
    ASSERT_GT(SnapshotRecords(checkpoint_lsn), 1u) << "this cabin fits one record, so the "
                                                     "chunking path is not under test at all";

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().assertions[0].groups_restored, groups)
        << "a chunk was lost, so the base under-counts and an admission check on it is wrong";
    EXPECT_EQ(rebuilt.group_count(), groups);
}

// **A record is bounded by the ring as well as the segment** (AZ-S3). At the
// production defaults - a 64 MiB segment and a 1 MiB ring - a chunk cut to
// the segment is one the ring refuses whole, so a cabin with more than a
// ring's worth of headers had every run refused at its first chunk: every
// core-0 checkpoint after it failed, and so did the next mount's completion
// checkpoint. The fixture's 64 KiB segment is smaller than any ring, which
// is why no other cell here reached it. A 4 MiB segment has the production
// shape - larger than the default ring - at a sixteenth of the scan.
TEST_F(AssertionRecoverTest, ACabinPastOneRingOfHeadersIsCutToChunksTheRingCanStage) {
    constexpr std::uint64_t kSegmentPastTheRing = 4 * 1024 * 1024;
    auto device = wal::MemoryLogDevice::Create(kSegmentPastTheRing);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto manager = wal::WalManager::Open(device.value().get(), clock_, /*core_id=*/0);
    ASSERT_TRUE(manager.ok()) << manager.status().message();
    wal::WalManager& wal = *manager.value();
    ASSERT_LT(wal.stream()->ring_capacity(), kSegmentPastTheRing)
        << "the default ring is no longer below the segment";

    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    for (int i = 0; i < 5000; ++i) {
        live.EnsureGroupId(Key("group-" + std::string(300, 'k') + std::to_string(i)));
    }
    wal::AssertionCabinSnapshot cabin;
    cabin.assertion_id = kAssertionId;
    std::size_t bytes = 0;
    for (const BoundCabin::GroupSnapshot& g : live.SnapshotGroups()) {
        bytes += wal::AssertSnapshotGroupBytes(g.key.size());
        cabin.groups.push_back(wal::AssertionSnapshotGroup{g.group_id, g.count, g.sum, g.key});
    }
    ASSERT_GT(bytes, wal.stream()->ring_capacity()) << "the cabin fits one ring";

    const Status logged = wal::LogAssertionSnapshot(wal, cabin);
    ASSERT_TRUE(logged.ok()) << logged.message();
    ASSERT_TRUE(wal.Flush().ok());

    RecoverableAssertion a;
    a.assertion_id = kAssertionId;
    a.root_page_id = kCabinPage;
    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    a.cabin = &rebuilt;
    const std::array<RecoverableAssertion, 1> list = {a};
    auto report = RecoverAssertions(*device.value(), /*core_id=*/0, /*from_lsn=*/0, store_, list,
                                    /*log=*/nullptr);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(rebuilt.group_count(), live.group_count());
}

// **A checkpoint that meets a cabin no run can carry completes** (AZ-S3,
// AZ-R3). The writer refuses such a cabin before its first chunk; until
// AZ-S3 that refusal failed the whole checkpoint, so core 0 never completed
// another one while the assertion lived and the next mount's completion
// checkpoint failed the mount. Now the registry evicts the assertion and
// marks it unenforceable inside the snapshot's hold - its relation's writes
// are refused `CannotEnforce` until DROP and CREATE - and the checkpoint
// carries every other assertion's run and publishes.
//
// Reached here by a group key longer than a record: admission refuses one
// on a registry that knows its budget, so a volume written before AZ-S3 is
// where a checkpoint meets it.
TEST_F(AssertionRecoverTest, ACheckpointMeetingACabinNoRunCanCarryCompletesAndFailsItClosed) {
    AssertionEnforcer registry(/*shared=*/true);
    LiveAssertion unsnapshottable;
    unsnapshottable.assertion_id = kAssertionId;
    unsnapshottable.target_oid = 4000;
    unsnapshottable.aggregate = BoundAggregate::kSum;
    unsnapshottable.cabin = BoundCabin(BoundAggregate::kSum, /*bound=*/1'000'000);
    unsnapshottable.cabin.EnsureGroupId(Key(std::string(wal_->usable_payload_bytes(), 'z')));
    registry.Adopt(std::move(unsnapshottable));
    LiveAssertion other;
    other.assertion_id = kAssertionId + 1;
    other.target_oid = 4001;
    other.aggregate = BoundAggregate::kSum;
    other.cabin = BoundCabin(BoundAggregate::kSum, /*bound=*/1'000'000);
    other.cabin.EnsureGroupId(Key("small"));
    registry.Adopt(std::move(other));

    class NoPages final : public wal::CheckpointTarget {
    public:
        std::vector<wal::CheckpointDirtyPage> DirtyTable() const override { return {}; }
        Status FlushPages(std::span<const PageId>) override { return Status::OK(); }
    };
    NoPages target;
    wal::NoActiveTransactions none;
    wal::InMemoryCheckpointAnchor anchor;
    wal::Checkpointer checkpointer(*wal_, target, none, anchor);
    checkpointer.SetAssertionSource(&registry);
    const Status completed = checkpointer.RunToCompletion();
    ASSERT_TRUE(completed.ok()) << completed.message();
    EXPECT_EQ(anchor.publishes(), 1u);

    EXPECT_FALSE(registry.Holds(kAssertionId)) << "an unsnapshottable cabin is still enforcing";
    EXPECT_TRUE(registry.CannotEnforce(4000)) << "its relation's writes are admitted unchecked";
    EXPECT_TRUE(registry.Holds(kAssertionId + 1));
    EXPECT_EQ(SnapshotRecords(anchor.anchor().checkpoint_lsn), 1u)
        << "the other assertion's run is missing";

    // The next mount, from this checkpoint: no base, so the assertion comes
    // up unrecovered - unenforceable, as it went down - and the mount's own
    // completion checkpoint has nothing of it to meet.
    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(anchor.anchor().checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_FALSE(report.value().assertions[0].recovered);
}

// Ported from AY-S8's superseded attempt (`266db2e`), with AZ-Q2's code.
TEST_F(AssertionRecoverTest, AGroupNoRecordCanCarryIsRefusedBeforeAnyChunkIsWritten) {
    // The writer cuts the run before it writes, so a too-large group anywhere
    // in the cabin - here the last - leaves no partial run in the log.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    live.EnsureGroupId(Key(std::string(wal_->usable_payload_bytes(), 'z')));
    wal::AssertionCabinSnapshot cabin;
    cabin.assertion_id = kAssertionId;
    for (const BoundCabin::GroupSnapshot& g : live.SnapshotGroups()) {
        cabin.groups.push_back(wal::AssertionSnapshotGroup{g.group_id, g.count, g.sum, g.key});
    }
    const wal::Lsn before = wal_->appended_lsn();
    EXPECT_EQ(wal::LogAssertionSnapshot(*wal_, cabin).code(), StatusCode::kNotImplemented);
    EXPECT_EQ(wal_->appended_lsn(), before) << "a chunk was written ahead of the refusal";
}

// `GROUP BY (v)` on `(id, v int64)`: one group per value.
std::vector<parser::AstValue> IntRow(std::int64_t v) {
    std::vector<parser::AstValue> row(1);
    row[0].type = parser::ValueType::kInt;
    row[0].int_val = v;
    return row;
}

wal::AssertionCabinSnapshot SnapshotOfGroups(std::int64_t groups) {
    BoundCabin cabin(BoundAggregate::kCount, /*bound=*/1'000'000);
    for (std::int64_t v = 0; v < groups; ++v) cabin.EnsureGroupId(EncodeGroupKey(IntRow(v)));
    wal::AssertionCabinSnapshot out;
    for (const BoundCabin::GroupSnapshot& g : cabin.SnapshotGroups()) {
        out.groups.push_back(wal::AssertionSnapshotGroup{g.group_id, g.count, g.sum, g.key});
    }
    return out;
}

// **The writer's chunk-count door at its threshold** (AZ-S3), through the
// budget rather than 4 GB of headers: a budget of one group per chunk makes
// the run's chunk count its group count. No cell reached this refusal before.
TEST(AssertionSnapshotDoorTest, TheWritersChunkCountIsRefusedOnePastWhatARunCanCount) {
    const std::size_t cost = wal::AssertSnapshotGroupBytes(EncodeGroupKey(IntRow(0)).size());
    const std::size_t one_per_chunk = wal::kAssertSnapshotFixedSize + cost;
    const auto most = static_cast<std::int64_t>(wal::kMaxAssertSnapshotChunks);
    EXPECT_TRUE(wal::AssertionSnapshotFits(SnapshotOfGroups(most), one_per_chunk).ok());
    EXPECT_EQ(wal::AssertionSnapshotFits(SnapshotOfGroups(most + 1), one_per_chunk).code(),
              StatusCode::kNotImplemented);
}

// **The admission's chunk-count door at its threshold** (AZ-S3). A budget of
// the chunk's fixed part and two groups makes admission's floor exactly one
// group, so its bound is `kMaxAssertSnapshotChunks - 1` groups: the group
// that reaches it is admitted, the one past it is refused `NotImplemented`,
// and a row into a group that exists is admitted either way. Conservative by
// design - the writer's cut at that budget is two groups a chunk - and never
// the other way: every cabin admission lets form, the writer can carry.
TEST(AssertionSnapshotDoorTest, AGroupPastWhatARunCanCountIsRefusedAtAdmission) {
    const std::size_t cost = wal::AssertSnapshotGroupBytes(EncodeGroupKey(IntRow(0)).size());
    const std::size_t budget = wal::kAssertSnapshotFixedSize + 2 * cost;
    const auto bound = static_cast<std::int64_t>(wal::kMaxAssertSnapshotChunks - 1);
    EXPECT_TRUE(wal::AssertionSnapshotFits(SnapshotOfGroups(bound), budget).ok())
        << "admission's bound admits a cabin the writer refuses";

    const auto registry_of = [&](std::int64_t groups) {
        auto registry = std::make_unique<AssertionEnforcer>();
        LiveAssertion a;
        a.assertion_id = 5;
        a.target_oid = 4000;
        a.name = "cap";
        a.aggregate = BoundAggregate::kCount;
        a.group_cols = {1};
        a.group_col_names = {"v"};
        a.group_type_vals = {catalog::kTypeValInt64};
        a.cabin = BoundCabin(BoundAggregate::kCount, /*bound=*/1'000'000);
        for (std::int64_t v = 0; v < groups; ++v) a.cabin.EnsureGroupId(EncodeGroupKey(IntRow(v)));
        registry->Adopt(std::move(a));
        registry->SetRecordBudget(budget);
        return registry;
    };
    const auto admit = [](AssertionEnforcer& registry, std::int64_t v) {
        AssertionEnforcer::Hold hold;
        return registry.AdmitInsert(4000, IntRow(v), /*writer_txn=*/0, hold);
    };

    auto below = registry_of(bound - 1);
    EXPECT_TRUE(admit(*below, bound).ok()) << "the group that reaches the bound was refused";

    auto at = registry_of(bound);
    const Status refused = admit(*at, bound + 1);
    EXPECT_EQ(refused.code(), StatusCode::kNotImplemented) << refused.message();
    EXPECT_NE(refused.message().find("cap"), std::string::npos) << refused.message();
    EXPECT_TRUE(admit(*at, 7).ok()) << "a row into an existing group was refused";

    // A group another admission is about to open counts: with it held, the
    // last room is gone for a second new group.
    AssertionEnforcer::Hold opening;
    ASSERT_TRUE(below->AdmitInsert(4000, IntRow(bound), /*writer_txn=*/1, opening).ok());
    EXPECT_EQ(admit(*below, bound + 1).code(), StatusCode::kNotImplemented)
        << "two admissions each took the last room";

    at->SetRecordBudget(0);
    EXPECT_TRUE(admit(*at, bound + 1).ok()) << "a registry with no log refused a group";
}

// **A checkpoint's eviction refuses the rest of the statement it lands in**
// (AZ-S3's review). The dispatcher asks `CannotEnforce` once per statement;
// a checkpoint can now mark the relation between two of its rows, and the
// per-row admission answered "no live assertion" and admitted the rest
// unchecked. It asks again per row now, and refuses.
TEST(AssertionSnapshotDoorTest, AnEvictionMidStatementRefusesTheRowsAfterIt) {
    AssertionEnforcer registry(/*shared=*/true);
    LiveAssertion a;
    a.assertion_id = 5;
    a.target_oid = 4000;
    a.name = "cap";
    a.aggregate = BoundAggregate::kCount;
    a.group_cols = {1};
    a.group_col_names = {"v"};
    a.group_type_vals = {catalog::kTypeValInt64};
    a.cabin = BoundCabin(BoundAggregate::kCount, /*bound=*/1'000'000);
    a.cabin.EnsureGroupId(EncodeGroupKey(IntRow(1)));
    registry.Adopt(std::move(a));

    const auto admit = [&](std::int64_t v) {
        AssertionEnforcer::Hold hold;
        return registry.AdmitInsert(4000, IntRow(v), /*writer_txn=*/0, hold);
    };
    ASSERT_TRUE(admit(1).ok()) << "row 1, before the checkpoint";

    // A checkpoint whose record cannot carry even the one group.
    ASSERT_TRUE(registry
                    .VisitSnapshots(/*record_budget=*/wal::kAssertSnapshotFixedSize,
                                    [](const std::vector<wal::AssertionCabinSnapshot>& cabins) {
                                        EXPECT_TRUE(cabins.empty());
                                        return Status::OK();
                                    })
                    .ok());
    ASSERT_TRUE(registry.CannotEnforce(4000));

    const Status row2 = admit(1);
    EXPECT_EQ(row2.code(), StatusCode::kNotImplemented)
        << "the row after the eviction was admitted unchecked: " << row2.message();
}

// **A key past its records' `u16` length is refused at admission** (AZ-S3's
// review): at the production budget - about 1 MiB - the record would carry it,
// but `ASSERT_RESERVE`'s key length is a `u16`, so the reservation would fail
// after the row is placed.
TEST(AssertionSnapshotDoorTest, AKeyPastItsRecordsLengthFieldIsRefusedAtAdmission) {
    AssertionEnforcer registry;
    LiveAssertion a;
    a.assertion_id = 5;
    a.target_oid = 4000;
    a.name = "cap";
    a.aggregate = BoundAggregate::kCount;
    a.group_cols = {1};
    a.group_col_names = {"v"};
    a.group_type_vals = {catalog::kTypeValVarchar};
    a.cabin = BoundCabin(BoundAggregate::kCount, /*bound=*/1'000'000);
    registry.Adopt(std::move(a));
    registry.SetRecordBudget(std::size_t{1} << 20);

    const auto admit = [&](std::size_t length) {
        std::vector<parser::AstValue> row(1);
        row[0].type = parser::ValueType::kStr;
        row[0].str_val = std::string(length, 'k');
        AssertionEnforcer::Hold hold;
        return registry.AdmitInsert(4000, row, /*writer_txn=*/0, hold);
    };
    const Status refused = admit(wal::kMaxAssertKeyBytes + 1);
    EXPECT_EQ(refused.code(), StatusCode::kNotImplemented) << refused.message();
    EXPECT_TRUE(admit(1000).ok());
}

TEST_F(AssertionRecoverTest, ASecondCheckpointsSnapshotInRangeDoesNotFailThePass) {
    // The anchor is published at Complete(), so a crash *during* a later
    // checkpoint leaves that checkpoint's snapshot records inside the range the
    // anchor still points at. Restoring them a second time hits RestoreGroup's
    // duplicate-id refusal, and the whole pass then fails - which leaves every
    // assertion unenforcing after an ordinary crash.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    Write(live, Key("x"), 5, /*pk=*/1);
    const wal::Lsn first = Checkpoint(live);

    // An entry between the two checkpoints, so the fold has work whose result
    // proves the *first* snapshot is still the base.
    const std::uint32_t gx = live.Find(Key("x"))->group_id;
    const std::uint16_t after = Write(live, Key("x"), 7, /*pk=*/2);
    LogEntry(after, Key("x"), gx);
    Checkpoint(live);  // the second one, whose anchor the crash never published

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(first, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->sum, 12) << "5 from the first snapshot, 7 from the fold";
    EXPECT_EQ(x->count, 2);
    EXPECT_EQ(x->group_id, gx);
}

TEST_F(AssertionRecoverTest, AChunkedSnapshotRelinksEachEntryExactlyOnce) {
    // The linkage rebuild reads the cabin's pages once the base is whole, not
    // once per chunk: a later chunk's walk sees the earlier chunks' groups
    // already restored, so running it per chunk attaches their entries again -
    // and a duplicated pair is exactly what §7's VerifyAgainstEntries catches.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    Write(live, Key("x"), 5, /*pk=*/1);  // group id 1, so it lands in chunk one
    FillToChunk(live);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);
    ASSERT_GT(SnapshotRecords(checkpoint_lsn), 1u) << "one chunk cannot double-attach anything";

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().assertions[0].groups_restored, live.group_count());
    EXPECT_EQ(report.value().assertions[0].entries_attached, 1u)
        << "one entry on the pages, so one relink however many chunks the base took";

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->entries.size(), 1u);
}

// AY-S8 (AY-R7): a run is whole when it has as many chunks as it says, not
// when the first record that is not a snapshot arrives. Under one stream the
// second rule is false - another core's record lands between two chunks - and
// the chunks after it were skipped as "a later checkpoint's", so the base
// under-counted: a quiet wrong answer.
TEST_F(AssertionRecoverTest, AForeignRecordBetweenChunksLeavesTheBaseWhole) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 1u) << "one chunk has no gap to put a record in";

    const wal::Lsn from = AppendChunk(chunks[0]);
    AppendForeignRecord();
    for (std::size_t i = 1; i < chunks.size(); ++i) AppendChunk(chunks[i]);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(from, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(report.value().assertions[0].groups_restored, live.group_count())
        << "the chunks after the foreign record were dropped, so the base under-counts";
    EXPECT_EQ(rebuilt.group_count(), live.group_count());
}

TEST_F(AssertionRecoverTest, TheWriterNumbersEveryChunkOfItsRun) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 1u);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        auto decoded = wal::DecodeAssertSnapshot(chunks[i]);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();
        EXPECT_EQ(decoded.value().fields.chunk_index, i);
        EXPECT_EQ(decoded.value().fields.chunk_count, chunks.size());
    }

    BoundCabin empty(BoundAggregate::kSum, /*bound=*/1'000'000);
    const auto one = SnapshotChunks(empty);
    ASSERT_EQ(one.size(), 1u) << "an empty cabin still gets its one record";
    auto decoded = wal::DecodeAssertSnapshot(one[0]);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.chunk_count, 1u);
}

// AY-Q7 (B): a run the scan ends inside - a crash after some of its chunks - is
// not a base. Adopting it restores only the groups its chunks carried, and an
// admission check on that directory admits what the assertion forbids.
TEST_F(AssertionRecoverTest, ARunTornAtScanEndIsNotABase) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 1u) << "one chunk cannot be torn";

    const wal::Lsn from = AppendChunk(chunks[0]);
    for (std::size_t i = 1; i + 1 < chunks.size(); ++i) AppendChunk(chunks[i]);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(from, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_FALSE(report.value().assertions[0].recovered)
        << "a run missing its last chunk was adopted as a base";
    EXPECT_EQ(rebuilt.group_count(), 0u) << "a discarded run leaves nothing restored";
    EXPECT_EQ(report.value().assertions[0].partial_runs_discarded, 1u);
}

// A run cut short in a live log - `LogAssertionSnapshot` refusing mid-run, or a
// publish run a crash cut ahead of a later checkpoint's - is not a base, and
// the whole run after it is. Closing the partial one at the next foreign record
// made it the base and skipped the whole one.
TEST_F(AssertionRecoverTest, APartialRunIsDiscardedAndTheWholeRunAfterItIsTheBase) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 1u) << "one chunk cannot be cut short";

    const wal::Lsn from = AppendChunk(chunks[0]);
    AppendForeignRecord();
    for (const auto& chunk : chunks) AppendChunk(chunk);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(from, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(rebuilt.group_count(), live.group_count())
        << "the partial run was taken as the base and the whole one skipped";
    EXPECT_EQ(report.value().assertions[0].partial_runs_discarded, 1u);
}

// The scan starts at `redo_start_lsn`, which can fall between two chunks of an
// earlier checkpoint's run. The chunks it meets are that run's tail, not a
// run, and the whole run after them is the base. Taking the tail finished the
// run by its count and skipped the whole one as "a later checkpoint's".
TEST_F(AssertionRecoverTest, ARunWhoseStartPrecedesTheScanIsSkippedAndTheNextWholeRunIsTheBase) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 1u) << "one chunk has no tail";

    AppendChunk(chunks[0]);
    const wal::Lsn from = AppendChunk(chunks[1]);
    for (std::size_t i = 2; i < chunks.size(); ++i) AppendChunk(chunks[i]);
    AppendForeignRecord();
    for (const auto& chunk : chunks) AppendChunk(chunk);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(from, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(rebuilt.group_count(), live.group_count())
        << "the tail of a run begun before the scan was taken as the base";
    EXPECT_EQ(report.value().assertions[0].partial_runs_discarded, 0u)
        << "a tail is skipped, not counted as a run cut short";
}

// A chunk out of its run's order - here chunk 1 twice - is bytes no writer
// produces (one latch writes a run whole). The run is discarded rather than
// restored twice, which would fail the whole pass on a duplicate group id and
// leave every assertion unenforcing; the whole run after it is the base.
TEST_F(AssertionRecoverTest, AChunkOutOfSequenceDiscardsItsRun) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(live);
    for (int i = 0; i < 700; ++i) {  // a third chunk, so chunk 1 is not the last
        live.EnsureGroupId(Key("more-" + std::string(60, 'k') + std::to_string(i)));
    }
    const auto chunks = SnapshotChunks(live);
    ASSERT_GT(chunks.size(), 2u) << "needs a chunk 1 that is not the last";

    const wal::Lsn from = AppendChunk(chunks[0]);
    AppendChunk(chunks[1]);
    AppendChunk(chunks[1]);
    for (const auto& chunk : chunks) AppendChunk(chunk);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(from, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered);
    EXPECT_EQ(report.value().assertions[0].partial_runs_discarded, 1u);
    EXPECT_EQ(rebuilt.group_count(), live.group_count());
}

// **A create's publish run and a checkpoint's run of the same id never
// interleave** (AZ-S2, AZ-R2). Recovery keys runs by id alone, so two runs
// of one id crossed chunk by chunk - C0 C1 P0 C2 P1 P2 - are both discarded,
// and the assertion comes up unenforcing. `AdoptLogged` holds the directory
// latch from before the publish run until the adoption, and a checkpoint
// snapshots under the same latch, so the second run waits for the first.
//
// Each writer stops where the other's chunk would have to land for the
// crossing: the create after announcing itself and after its first chunk,
// the checkpoint after its second. With the one hold, each wait of the
// create's times out - the checkpoint is parked on the latch - and the log
// reads P0 P1 P2 C0 C1 C2.
//
// **Mutation**, measured: `AdoptLogged` releasing the latch between the
// adoption and the log, killed - the log reads C0 C1 P0 C2 P1 P2.
TEST_F(AssertionRecoverTest, ACreatesPublishRunAndACheckpointsRunOfItNeverInterleave) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/1'000'000);
    FillToChunk(cabin);
    for (int i = 0; i < 700; ++i) {  // a third chunk, so a run has a middle
        cabin.EnsureGroupId(Key("more-" + std::string(60, 'k') + std::to_string(i)));
    }
    const auto chunks = SnapshotChunks(cabin);
    ASSERT_GT(chunks.size(), 2u) << "a run of two chunks has no middle to cross at";
    const std::size_t groups = cabin.group_count();

    LiveAssertion created;
    created.assertion_id = kAssertionId;
    created.target_oid = 4000;
    created.aggregate = BoundAggregate::kSum;
    created.cabin = std::move(cabin);
    AssertionEnforcer registry(/*shared=*/true);

    std::mutex m;
    std::condition_variable cv;
    bool announced = false;
    bool checkpoint_mid_run = false;
    bool publish_began = false;
    bool checkpoint_done = false;
    const auto signal = [&](bool& flag) {
        const std::lock_guard<std::mutex> lock(m);
        flag = true;
        cv.notify_all();
    };
    // Bounded: with the one hold, the create's waits are for a checkpoint
    // parked on the latch the create holds.
    const auto await = [&](const bool& flag) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return flag; });
    };

    std::thread create([&] {
        const Status s = registry.AdoptLogged(
            std::move(created), [&](const std::vector<wal::AssertionCabinSnapshot>&) -> Status {
                signal(announced);
                (void)await(checkpoint_mid_run);
                AppendChunk(chunks[0]);
                signal(publish_began);
                (void)await(checkpoint_done);
                for (std::size_t i = 1; i < chunks.size(); ++i) AppendChunk(chunks[i]);
                return Status::OK();
            });
        EXPECT_TRUE(s.ok()) << s.message();
    });

    // Core 0's checkpoint, once the create has begun. Unbounded, and no
    // `ASSERT` before the join: the create thread holds references to this
    // frame.
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return announced; });
    }
    const Status snapshot = registry.VisitSnapshots(
        wal_->usable_payload_bytes(), [&](const std::vector<wal::AssertionCabinSnapshot>& cabins) -> Status {
            EXPECT_EQ(cabins.size(), 1u) << "the checkpoint ran without the created assertion";
            AppendChunk(chunks[0]);
            AppendChunk(chunks[1]);
            signal(checkpoint_mid_run);
            (void)await(publish_began);
            for (std::size_t i = 2; i < chunks.size(); ++i) AppendChunk(chunks[i]);
            signal(checkpoint_done);
            return Status::OK();
        });
    create.join();
    ASSERT_TRUE(snapshot.ok()) << snapshot.message();

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(/*from_lsn=*/0, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(report.value().assertions[0].recovered)
        << "the two runs crossed and recovery discarded both";
    EXPECT_EQ(rebuilt.group_count(), groups);
}

TEST_F(AssertionRecoverTest, LinkageTheWalkAndTheFoldBothAttachedIsReconciled) {
    // Both steps are right in isolation and they overlap: the page walk attaches
    // every entry the pages carry, and the fold appends again for a
    // post-checkpoint entry whose group already existed. Left alone, §7's
    // VerifyAgainstEntries double-counts that entry and answers Corruption on a
    // directory whose aggregate is correct.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    Write(live, Key("x"), 5, /*pk=*/1);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    const std::uint32_t gx = live.Find(Key("x"))->group_id;
    const std::uint16_t after = Write(live, Key("x"), 7, /*pk=*/2);
    LogEntry(after, Key("x"), gx);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().assertions[0].duplicate_links_dropped, 1u)
        << "the post-checkpoint entry was attached by the walk and by the fold";

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->sum, 12);
    EXPECT_EQ(x->count, 2);
    ASSERT_EQ(x->entries.size(), 2u) << "one pair per entry, not one per attachment";
    EXPECT_EQ(x->entries.size(), live.Find(Key("x"))->entries.size());

    // The proof §5.2 names, over the rebuild: header == sum(entries). It is this
    // that the duplicate broke.
    auto read = [this](PageId page_id, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        auto page = store_.Get(page_id);
        if (!page.ok()) return page.status();
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        return view.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok())
        << "a recovered cabin must satisfy the check the spec calls the proof";
}

// The AS6b decision of 2026-08-12 (`docs/spec/assertion.md` §7), and the case
// no fold can reach: the abort happens **before** the checkpoint, so its
// ASSERT_ROLLBACK is not in the scan's range and the page walk is the only
// thing that sees the entry. Before `kEntryOrphaned` the walk could not tell it
// from a live one, so a recovered directory carried an entry the live one had
// dropped - the aggregate stayed right (snapshot + folded deltas) and §5.2's
// proof, the one check that would have caught a real divergence, reported
// Corruption on a directory that was correct.
TEST_F(AssertionRecoverTest, AnAbortBeforeTheCheckpointLeavesNoEntryForTheWalkToRelink) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    Write(live, Key("x"), 5, /*pk=*/1);

    // The reservation that aborts. The live abort path: the directory drops the
    // linkage, the page keeps the bytes and takes the mark
    // (`AssertionEnforcer::AbortTxn`).
    const std::uint16_t rolled_back = Write(live, Key("x"), 7, /*pk=*/2);
    ASSERT_TRUE(live.Unapply(Key("x"), 7, kCabinPage, rolled_back).ok());
    MarkOrphaned(rolled_back);

    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->sum, 5);
    EXPECT_EQ(x->count, 1);
    ASSERT_EQ(x->entries.size(), 1u) << "the orphan was relinked; the walk cannot tell it apart";
    EXPECT_EQ(x->entries.size(), live.Find(Key("x"))->entries.size());

    // The bytes are still there - abort has never shrunk a page, and the slot
    // stays the recorded leak that rides on purge. Only the linkage is gone.
    auto page = store_.Get(kCabinPage);
    ASSERT_TRUE(page.ok());
    auto view = BoundCabinPage::Open(page.value().bytes());
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().entry_count(), 2);

    auto read = [this](PageId page_id, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        auto p = store_.Get(page_id);
        if (!p.ok()) return p.status();
        auto v = BoundCabinPage::Open(p.value().bytes());
        if (!v.ok()) return v.status();
        return v.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok())
        << "§5.2's proof must hold on a recovered cabin, which is where it earns its keep";
}

// The mirror of the test above, and the case the mark's *durability* creates:
// the abort happens **after** the checkpoint, so the fold does carry its
// ASSERT_ROLLBACK - and the marked page reached the device before the crash,
// which is the ordinary shape of a crash during a later checkpoint (that
// checkpoint flushes the page, then dies before Complete() moves the anchor).
//
// The walk then skips the entry, so the pair `Unapply` looks for by name is not
// in the group, and the compensation that the base snapshot *requires* - it was
// taken while the reservation was live, so it counts the delta - answered
// NotFound and failed the whole recovery pass. An assertion left unenforcing
// after an ordinary crash is the failure RC07 exists to prevent.
TEST_F(AssertionRecoverTest, AnAbortAfterTheCheckpointStillCompensatesWithTheMarkOnDisk) {
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1000);
    Write(live, Key("x"), 5, /*pk=*/1);
    const std::uint16_t rolled_back = Write(live, Key("x"), 7, /*pk=*/2);

    // The checkpoint sees the reservation live: its snapshot carries 12 over 2.
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    // The abort, after it, in `AssertionEnforcer::AbortTxn`'s order: the
    // directory drops the linkage, the page takes the mark, the record is
    // appended.
    ASSERT_TRUE(live.Unapply(Key("x"), 7, kCabinPage, rolled_back).ok());
    MarkOrphaned(rolled_back);
    LogRollback(rolled_back, Key("x"), /*delta=*/7);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();

    const GroupHeader* x = rebuilt.Find(Key("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->sum, 5) << "the snapshot's 12 minus the 7 the fold compensated";
    EXPECT_EQ(x->count, 1);
    ASSERT_EQ(x->entries.size(), 1u) << "the aborted entry is no group's, on either path";
    EXPECT_EQ(x->sum, live.Find(Key("x"))->sum) << "the rebuild disagrees with the live directory";

    auto read = [this](PageId page_id, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        auto p = store_.Get(page_id);
        if (!p.ok()) return p.status();
        auto v = BoundCabinPage::Open(p.value().bytes());
        if (!v.ok()) return v.status();
        return v.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok())
        << "§5.2's proof must hold on a recovered cabin, which is where it earns its keep";
}

// ---- The mount wiring (RC07): a real assertion, resumed --------------------

// Everything above drives the pass directly. This drives it the way a mount
// does: a real `CREATE ASSERTION` through the dispatcher, a real checkpoint
// through the registry-as-snapshot-source, then a **fresh** registry resumed
// from the log - which is what a restart is, minus the process boundary.
class AssertionResumeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto page_device = storage::MemoryPageDevice::Create(/*extent_pages=*/64,
                                                             /*initial_pages=*/64);
        ASSERT_TRUE(page_device.ok()) << page_device.status().message();
        page_device_ = std::move(page_device.value());

        auto log_device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log_device.ok()) << log_device.status().message();
        log_device_ = std::move(log_device.value());

        auto manager = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        wal_ = std::move(manager.value());

        auto store = storage::DevicePageStore::Open(*page_device_, server::kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetWalGate(wal_.get());

        auto boot = bootstrap::BootstrapDatabase(*store_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        trx_ids_.emplace(boot_->superblock);
        undo_.emplace(*store_, wal_.get());
        txn_.emplace(*trx_ids_, *undo_, *store_, wal_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr,
                            /*clock=*/nullptr, wal_.get(), wal::DurabilityClass::kStrict,
                            Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*txn_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    std::unique_ptr<storage::MemoryPageDevice> page_device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    sched::ManualClock clock_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txn_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

TEST_F(AssertionResumeTest, AFreshRegistryResumesEnforcingWithTheRecoveredAggregate) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, branch int32, amount int64)").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON accounts GROUP BY (branch) "
                  "CHECK SUM(amount) <= 100").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO accounts VALUES (1, 40)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO accounts VALUES (1, 30)").substr(0, 8), "INSERTED");

    // The checkpoint, with the registry as AS6a's source - the same wiring
    // `Expeditor::Open` does, which is what makes the base below the one a real
    // mount would find.
    storage::PageStoreCheckpointTarget target(*store_);
    wal::NoActiveTransactions none;
    wal::InMemoryCheckpointAnchor anchor;
    wal::Checkpointer checkpointer(*wal_, target, none, anchor);
    checkpointer.SetAssertionSource(&dispatcher_->assertions());
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    ASSERT_TRUE(wal_->Flush().ok());

    // The restart: a registry that holds nothing, refilled from the log.
    AssertionEnforcer fresh;
    EXPECT_TRUE(fresh.empty());
    const server::MountRecovery report = server::ResumeAssertionsAfterRecovery(
        boot_->catalog, *store_, *log_device_, /*stream_core=*/0,
        anchor.anchor().checkpoint_lsn, fresh, server::MountRecovery{}, /*log=*/nullptr);

    EXPECT_EQ(report.assertions_enforcing, 1u);
    EXPECT_EQ(report.assertions_unrecovered, 0u);
    EXPECT_FALSE(fresh.empty()) << "SHOW ASSERTIONS derives enforcing=1 from exactly this";

    // And the aggregate is the **recovered** one, not zero: the admission
    // boundary has to answer identically either side of the restart, which is
    // RC07's done-when. A directory at zero would admit all three of these.
    std::vector<parser::AstValue> row(2);
    row[0].type = parser::ValueType::kInt;
    row[1].type = parser::ValueType::kInt;
    const auto admit = [&](std::int64_t amount) {
        row[0].int_val = 1;  // branch
        row[1].int_val = amount;
        AssertionEnforcer::Hold hold;  // given back on return: each probe is its own
        return fresh.AdmitInsert(4000, row, /*writer_txn=*/0, hold);
    };
    EXPECT_EQ(admit(50).code(), StatusCode::kAssertionViolation) << "70 + 50 > 100";
    EXPECT_TRUE(admit(30).ok()) << "70 + 30 == 100, exactly at the bound";
}

TEST_F(AssertionResumeTest, AnAssertionCreatedAfterTheLastCheckpointStillRecovers) {
    // The permanence trap: an unbased assertion stays out of the registry, and
    // the completion checkpoint snapshots only the registry - so without a base
    // written at CREATE, `enforcing=0` would survive every later restart until
    // DROP + CREATE. With a long checkpoint interval that is every new assertion.
    //
    // No checkpoint is taken anywhere in this test. The only base in the stream
    // is the one CREATE ASSERTION wrote at its publish.
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, branch int32, amount int64)").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON accounts GROUP BY (branch) "
                  "CHECK SUM(amount) <= 100").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO accounts VALUES (1, 40)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO accounts VALUES (1, 30)").substr(0, 8), "INSERTED");
    ASSERT_TRUE(wal_->Flush().ok());

    AssertionEnforcer fresh;
    const server::MountRecovery report = server::ResumeAssertionsAfterRecovery(
        boot_->catalog, *store_, *log_device_, /*stream_core=*/0,
        /*from_lsn=*/0, fresh,
        server::MountRecovery{}, /*log=*/nullptr);

    EXPECT_EQ(report.assertions_enforcing, 1u);
    EXPECT_EQ(report.assertions_unrecovered, 0u);

    // And with the right aggregate: the create-time base carries the built
    // groups (none here), and the two inserts' records fold onto it.
    std::vector<parser::AstValue> row(2);
    row[0].type = parser::ValueType::kInt;
    row[1].type = parser::ValueType::kInt;
    const auto admit = [&](std::int64_t amount) {
        row[0].int_val = 1;
        row[1].int_val = amount;
        AssertionEnforcer::Hold hold;  // given back on return: each probe is its own
        return fresh.AdmitInsert(4000, row, /*writer_txn=*/0, hold);
    };
    EXPECT_EQ(admit(50).code(), StatusCode::kAssertionViolation) << "70 + 50 > 100";
    EXPECT_TRUE(admit(30).ok()) << "70 + 30 == 100";
}

TEST_F(AssertionResumeTest, AGroupKeyNoRecordCanCarryIsRefusedAtAdmission) {
    // **The survey's question, answered by SQL** (AZ-S3): a `GROUP BY` over
    // spilled `varchar`s reaches a key past one record - eight values of
    // 8,000 bytes against this fixture's 61 KiB record. The row is refused at
    // admission, before anything is placed, and `NOT_IMPLEMENTED` says
    // why: the bound is the log format's, not the data's fault.
    std::string columns;
    std::string group;
    for (char c = 'a'; c <= 'h'; ++c) {
        columns += std::string(", ") + c + " varchar";
        group += std::string(group.empty() ? "" : ", ") + c;
    }
    ASSERT_EQ(Run("CREATE TABLE wide (id int64" + columns + ")").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON wide GROUP BY (" + group + ") CHECK COUNT(*) <= 5")
                  .substr(0, 7),
              "CREATED");

    const std::string huge = "'" + std::string(8000, 'v') + "'";
    std::string values;
    for (int i = 0; i < 8; ++i) values += std::string(i == 0 ? "" : ", ") + huge;
    const std::string refused = Run("INSERT INTO wide VALUES (" + values + ")");
    EXPECT_NE(refused.find("NOT_IMPLEMENTED"), std::string::npos) << refused.substr(0, 300);
    EXPECT_NE(refused.find("cap"), std::string::npos) << refused.substr(0, 300);
    EXPECT_EQ(Run("SELECT COUNT(*) FROM wide"), "count(*)\\n0") << "the refused row was placed";

    // A key that fits is admitted as before.
    EXPECT_EQ(Run("INSERT INTO wide VALUES ('a', 'b', 'c', 'd', 'e', 'f', 'g', 'h')").substr(0, 8),
              "INSERTED");
}

TEST_F(AssertionResumeTest, WithNoBaseInRangeTheAssertionIsNotAdoptedAtAll) {
    // **This test's premise changed when CREATE ASSERTION started writing its own
    // base.** It used to produce "no snapshot" by simply not checkpointing; that
    // is now impossible, and the test failed - correctly - the moment the create
    // path closed the hole. The property it guards is unchanged and still worth
    // guarding, so it is produced the only way it can now arise: a base that has
    // fallen *out of the scan range*, which is what a later checkpoint's redo
    // start does to an older record.
    //
    // Not adopted, because a cabin at zero admits every write: reporting
    // `enforcing=1` for it would be worse than the honest `enforcing=0`.
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, branch int32, amount int64)").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON accounts GROUP BY (branch) "
                  "CHECK SUM(amount) <= 100").substr(0, 7),
              "CREATED");

    // Everything the create wrote, including its base, is now below this point.
    const wal::Lsn after_create = wal_->appended_lsn();

    ASSERT_EQ(Run("INSERT INTO accounts VALUES (1, 40)").substr(0, 8), "INSERTED");
    ASSERT_TRUE(wal_->Flush().ok());

    AssertionEnforcer fresh;
    const server::MountRecovery report = server::ResumeAssertionsAfterRecovery(
        boot_->catalog, *store_, *log_device_, /*stream_core=*/0, after_create,
        fresh,
        server::MountRecovery{}, /*log=*/nullptr);

    EXPECT_EQ(report.assertions_enforcing, 0u);
    EXPECT_EQ(report.assertions_unrecovered, 1u);
    EXPECT_TRUE(fresh.empty());
}

}  // namespace
}  // namespace kds::exec
