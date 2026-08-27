#include "kds/exec/assertion_recover.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
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
            std::vector<wal::AssertionCabinSnapshot> SnapshotAssertions() const override {
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
                return {out};
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
    // A cabin's group count is bounded by the data, so the snapshot chunks - and
    // the loader is additive over the chunks, which is the property that makes a
    // continuation flag unnecessary.
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
    for (int i = 0; i < 700; ++i) {
        live.EnsureGroupId(Key("group-" + std::string(60, 'k') + std::to_string(i)));
    }
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
    for (int i = 0; i < 700; ++i) {
        live.EnsureGroupId(Key("group-" + std::string(60, 'k') + std::to_string(i)));
    }
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
        boot_->catalog, *store_, *log_device_, /*core_id=*/0,
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
        return fresh.AdmitInsert(4000, row);
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
        boot_->catalog, *store_, *log_device_, /*core_id=*/0, /*from_lsn=*/0, fresh,
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
        return fresh.AdmitInsert(4000, row);
    };
    EXPECT_EQ(admit(50).code(), StatusCode::kAssertionViolation) << "70 + 50 > 100";
    EXPECT_TRUE(admit(30).ok()) << "70 + 30 == 100";
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
        boot_->catalog, *store_, *log_device_, /*core_id=*/0, after_create, fresh,
        server::MountRecovery{}, /*log=*/nullptr);

    EXPECT_EQ(report.assertions_enforcing, 0u);
    EXPECT_EQ(report.assertions_unrecovered, 1u);
    EXPECT_TRUE(fresh.empty());
}

}  // namespace
}  // namespace kds::exec
