#include "kds/exec/assertion_recover.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wal/checkpointer.hpp"
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
        ASSERT_TRUE(BoundCabinPage::Format(page.value()).ok());
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
        auto view = BoundCabinPage::Open(page.value());
        EXPECT_TRUE(view.ok());
        auto index = view.value().Append(entry);
        EXPECT_TRUE(index.ok()) << index.status().message();
        EXPECT_TRUE(live.Apply(key, value, kCabinPage, index.value()).ok());
        return index.value();
    }

    // ASSERT_BUILD for an entry already on the page, so the fold has a record to
    // apply for the post-checkpoint half.
    void LogEntry(std::uint16_t index, const std::string& key, std::uint32_t group_id) {
        auto page = store_.Get(kCabinPage);
        ASSERT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value());
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

    // The checkpoint, through the real Checkpointer and the real seam - so the
    // snapshot under test is the one a mount would actually find.
    wal::Lsn Checkpoint(const BoundCabin& live) {
        class Source final : public wal::AssertionSnapshotSource {
        public:
            explicit Source(const BoundCabin& cabin) : cabin_(cabin) {}
            std::vector<wal::AssertionCabinSnapshot> SnapshotAssertions() const override {
                wal::AssertionCabinSnapshot out;
                out.assertion_id = kAssertionId;
                keys_.clear();
                for (const BoundCabin::GroupSnapshot& g : cabin_.SnapshotGroups()) {
                    keys_.push_back(g.key);
                }
                std::size_t i = 0;
                for (const BoundCabin::GroupSnapshot& g : cabin_.SnapshotGroups()) {
                    wal::SnapshotGroupEntry entry;
                    entry.group_id = g.group_id;
                    entry.count = g.count;
                    entry.sum = g.sum;
                    entry.key = std::as_bytes(std::span<const char>(keys_[i].data(),
                                                                    keys_[i].size()));
                    out.groups.push_back(entry);
                    ++i;
                }
                return {out};
            }

        private:
            const BoundCabin& cabin_;
            mutable std::vector<std::string> keys_;  // the spans must outlive the call
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
        auto view = BoundCabinPage::Open(page.value());
        if (!view.ok()) return view.status();
        return view.value().Read(index);
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok());
}

TEST_F(AssertionRecoverTest, RecordsWithNoSnapshotAreCountedAndTheAssertionStaysUnrecovered) {
    // Records but no base: folding them would produce aggregates that are too
    // small, and an admission check on those admits a violating write. So the
    // assertion is reported unrecovered - `enforcing=0` - rather than enforcing
    // wrongly.
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
    // A cabin's group count is bounded by the data, so the snapshot chunks - and
    // the loader is additive over the chunks, which is the property that makes a
    // continuation flag unnecessary.
    //
    // Groups only, no entries: 600 headers with these keys exceed what one
    // record's payload can hold, which is the boundary under test. Entries would
    // add nothing to it and would need a chain of pages (one holds 254), so the
    // linkage rebuild is left to the test above that is about it.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/1'000'000);
    for (int i = 0; i < 600; ++i) {
        live.EnsureGroupId(Key("group-" + std::string(60, 'k') + std::to_string(i)));
    }
    const std::size_t groups = live.group_count();
    ASSERT_EQ(groups, 600u);
    const wal::Lsn checkpoint_lsn = Checkpoint(live);

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/1'000'000);
    auto report = Recover(checkpoint_lsn, rebuilt);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().assertions[0].groups_restored, groups)
        << "a chunk was lost, so the base under-counts and an admission check on it is wrong";
    EXPECT_EQ(rebuilt.group_count(), groups);
}

}  // namespace
}  // namespace kds::exec
