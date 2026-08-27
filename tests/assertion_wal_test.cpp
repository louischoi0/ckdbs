#include "kds/exec/assertion_replay.hpp"

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "kds/exec/bound_cabin.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The assertion WAL records and their replay (docs/spec/assertion.md §7,
// workplan AST05).
//
// The test worth reading is the fold: a live run mutates a page and a group
// directory through the ordinary APIs while encoding the record each
// mutation would log, and replay folds those records over a fresh store and
// an empty directory. The claim §7 makes - "replay restores group headers
// and entries exactly" - is then checked in its strongest form: the page
// byte for byte (the undo-page test's precedent), the directory header by
// header, and the admission boundary answering identically on both sides,
// which is the "no gap where a violating write could be admitted" half.
//
// What is deliberately *not* here: the crash matrix. The S-2 recovery
// harness does not exist - nothing in this engine reads a log back yet - so
// per the workplan's gate these are the replay unit tests landed now, and
// the crash-at-every-boundary matrix is registered as a SIM-series
// follow-up in the workplan.

namespace kds::exec {
namespace {

using storage::InMemoryPageStore;
using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryBytes;
using storage::cabin::kEntryReserved;

constexpr std::uint64_t kAssertionId = 42;
constexpr PageId kEntryPage = 100;

parser::AstValue Int(std::int64_t v) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = v;
    return value;
}

std::string KeyOf(std::int64_t group) { return EncodeGroupKey({Int(group)}); }

std::span<const std::byte> Bytes(const std::string& s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

BoundCabinEntry Entry(std::uint64_t pk, std::int64_t value, std::uint8_t flags) {
    BoundCabinEntry entry;
    entry.pk = pk;
    entry.value = value;
    entry.flags = flags;
    return entry;
}

std::array<std::byte, kEntryBytes> EncodedEntry(const BoundCabinEntry& entry) {
    std::array<std::byte, kEntryBytes> buf{};
    EXPECT_TRUE(storage::cabin::EncodeEntry(entry, buf).ok());
    return buf;
}

// Encodes a whole record - envelope and payload - the way the emitting site
// will, so replay decodes exactly what a stream would carry.
std::vector<std::byte> WholeRecord(wal::RecordType type, std::uint64_t txn_id, PageId page_id,
                                   std::span<const std::byte> payload,
                                   std::uint8_t flags = 0) {
    std::vector<std::byte> buf(wal::EncodedRecordSize(payload.size()));
    wal::RecordSpec spec;
    spec.type = type;
    spec.txn_id = txn_id;
    spec.page_id = page_id;
    spec.flags = flags;
    auto used = wal::EncodeRecord(buf, spec, /*lsn=*/4096, payload);
    EXPECT_TRUE(used.ok()) << used.status().message();
    return buf;
}

std::vector<std::byte> EntryRecord(wal::RecordType type, std::uint64_t txn_id,
                                   std::uint16_t index, const BoundCabinEntry& entry,
                                   const std::string& key) {
    const auto entry_bytes = EncodedEntry(entry);
    std::vector<std::byte> payload(wal::kAssertEntryFixedSize + kEntryBytes + key.size());
    wal::AssertEntryPayload fields{};
    fields.assertion_id = kAssertionId;
    fields.index = index;
    // AS6a: from the entry, never a separate argument - the record and the
    // entry bytes disagreeing about the group is exactly what replay's
    // AdoptGroupId refuses, so a test must not be able to produce it.
    fields.group_id = entry.group_id;
    auto used = wal::EncodeAssertEntry(payload, fields, entry_bytes, Bytes(key));
    EXPECT_TRUE(used.ok()) << used.status().message();
    return WholeRecord(type, txn_id, kEntryPage, payload);
}

std::vector<std::byte> CommitRecord(std::uint64_t txn_id,
                                    std::span<const std::uint16_t> indexes) {
    std::vector<std::byte> payload(wal::kAssertCommitFixedSize + indexes.size() * 2);
    wal::AssertCommitPayload fields{};
    fields.assertion_id = kAssertionId;
    auto used = wal::EncodeAssertCommit(payload, fields, indexes);
    EXPECT_TRUE(used.ok()) << used.status().message();
    return WholeRecord(wal::RecordType::kAssertCommit, txn_id, kEntryPage, payload);
}

std::vector<std::byte> RollbackRecord(std::uint64_t txn_id, std::int64_t delta,
                                      std::uint16_t index, const std::string& key,
                                      bool departure = false) {
    std::vector<std::byte> payload(wal::kAssertRollbackFixedSize + key.size());
    wal::AssertRollbackPayload fields{};
    fields.assertion_id = kAssertionId;
    fields.delta = delta;
    fields.index = index;
    auto used = wal::EncodeAssertRollback(payload, fields, Bytes(key));
    EXPECT_TRUE(used.ok()) << used.status().message();
    return WholeRecord(wal::RecordType::kAssertRollback, txn_id, kEntryPage, payload,
                       departure ? wal::kAssertRollbackFlagDeparture : 0);
}

std::vector<std::byte> DropRecord(std::uint64_t assertion_id) {
    std::vector<std::byte> payload(wal::kAssertDropPayloadSize);
    wal::AssertDropPayload fields{assertion_id};
    auto used = wal::EncodeAssertDrop(payload, fields);
    EXPECT_TRUE(used.ok()) << used.status().message();
    return WholeRecord(wal::RecordType::kAssertDrop, wal::kNoTxnId, kEntryPage, payload);
}

// The recovery-side owner of the directories, as a test double: a map, the
// skip rule for an unknown id, and a drop that erases.
class MapContext : public AssertionReplayContext {
public:
    BoundCabin* CabinOf(std::uint64_t assertion_id) override {
        auto it = cabins_.find(assertion_id);
        return it == cabins_.end() ? nullptr : it->second;
    }
    void Drop(std::uint64_t assertion_id) override {
        cabins_.erase(assertion_id);
        ++drops_;
    }

    void Adopt(std::uint64_t assertion_id, BoundCabin* cabin) { cabins_[assertion_id] = cabin; }
    bool Holds(std::uint64_t assertion_id) const { return cabins_.count(assertion_id) != 0; }
    int drops() const { return drops_; }

private:
    std::unordered_map<std::uint64_t, BoundCabin*> cabins_;
    int drops_ = 0;
};

Status Fold(const std::vector<std::vector<std::byte>>& log, storage::PageStore& store,
            AssertionReplayContext& context) {
    for (const std::vector<std::byte>& buf : log) {
        auto record = wal::DecodeRecord(buf);
        if (!record.ok()) return record.status();
        if (Status s = ReplayAssertionRecord(record.value(), store, context); !s.ok()) return s;
    }
    return Status::OK();
}

// ---- Payload codecs ------------------------------------------------------

TEST(AssertPayloadTest, EntryPayloadRoundTripsAndLengthsComeFromTheSpans) {
    const BoundCabinEntry entry = Entry(77, -5, kEntryReserved);
    const auto entry_bytes = EncodedEntry(entry);
    const std::string key = KeyOf(9);

    std::vector<std::byte> buf(wal::kAssertEntryFixedSize + kEntryBytes + key.size());
    wal::AssertEntryPayload fields{};
    fields.assertion_id = kAssertionId;
    fields.index = 3;
    fields.entry_len = 999;  // ignored: the span is the truth
    fields.key_len = 999;
    auto used = wal::EncodeAssertEntry(buf, fields, entry_bytes, Bytes(key));
    ASSERT_TRUE(used.ok()) << used.status().message();
    EXPECT_EQ(used.value(), buf.size());

    auto decoded = wal::DecodeAssertEntry(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    const wal::DecodedAssertEntry& d = decoded.value();
    EXPECT_EQ(d.fields.assertion_id, kAssertionId);
    EXPECT_EQ(d.fields.index, 3);
    EXPECT_EQ(d.fields.entry_len, kEntryBytes);
    EXPECT_EQ(d.fields.key_len, key.size());
    ASSERT_EQ(d.entry.size(), kEntryBytes);
    EXPECT_EQ(std::memcmp(d.entry.data(), entry_bytes.data(), kEntryBytes), 0);
    ASSERT_EQ(d.key.size(), key.size());
    EXPECT_EQ(std::memcmp(d.key.data(), key.data(), key.size()), 0);

    const BoundCabinEntry back = storage::cabin::DecodeEntry(
        std::span<const std::byte, kEntryBytes>(d.entry.data(), kEntryBytes));
    EXPECT_EQ(back.pk, 77u);
    EXPECT_EQ(back.value, -5);
    EXPECT_TRUE(back.reserved());
}

TEST(AssertPayloadTest, CommitPayloadRoundTripsItsIndexList) {
    const std::array<std::uint16_t, 3> indexes = {2, 7, 253};
    std::vector<std::byte> buf(wal::kAssertCommitFixedSize + indexes.size() * 2);
    wal::AssertCommitPayload fields{};
    fields.assertion_id = kAssertionId;
    auto used = wal::EncodeAssertCommit(buf, fields, indexes);
    ASSERT_TRUE(used.ok()) << used.status().message();

    auto decoded = wal::DecodeAssertCommit(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.assertion_id, kAssertionId);
    ASSERT_EQ(decoded.value().fields.count, 3);
    EXPECT_EQ(decoded.value().index_at(0), 2);
    EXPECT_EQ(decoded.value().index_at(1), 7);
    EXPECT_EQ(decoded.value().index_at(2), 253);
}

TEST(AssertPayloadTest, RollbackPayloadRoundTripsANegativeDeltaExactly) {
    const std::string key = KeyOf(-3);
    std::vector<std::byte> buf(wal::kAssertRollbackFixedSize + key.size());
    wal::AssertRollbackPayload fields{};
    fields.assertion_id = kAssertionId;
    fields.delta = -9'000'000'000LL;
    fields.index = 12;
    auto used = wal::EncodeAssertRollback(buf, fields, Bytes(key));
    ASSERT_TRUE(used.ok()) << used.status().message();

    auto decoded = wal::DecodeAssertRollback(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fields.delta, -9'000'000'000LL);
    EXPECT_EQ(decoded.value().fields.index, 12);
    ASSERT_EQ(decoded.value().key.size(), key.size());
    EXPECT_EQ(std::memcmp(decoded.value().key.data(), key.data(), key.size()), 0);
}

TEST(AssertPayloadTest, DropPayloadRoundTrips) {
    std::vector<std::byte> buf(wal::kAssertDropPayloadSize);
    auto used = wal::EncodeAssertDrop(buf, wal::AssertDropPayload{kAssertionId});
    ASSERT_TRUE(used.ok()) << used.status().message();
    auto decoded = wal::DecodeAssertDrop(buf);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().assertion_id, kAssertionId);
}

TEST(AssertPayloadTest, ATruncatedTailIsCorruptionNotATornEnd) {
    // The envelope's CRC vouched for these bytes, so a key_len that runs
    // past the payload is intact and wrong (payload.hpp's third convention).
    const std::string key = KeyOf(9);
    std::vector<std::byte> buf(wal::kAssertEntryFixedSize + kEntryBytes + key.size());
    wal::AssertEntryPayload fields{};
    fields.assertion_id = kAssertionId;
    auto used =
        wal::EncodeAssertEntry(buf, fields, EncodedEntry(Entry(1, 1, 0)), Bytes(key));
    ASSERT_TRUE(used.ok());

    auto truncated = wal::DecodeAssertEntry(
        std::span<const std::byte>(buf.data(), buf.size() - 1));
    ASSERT_FALSE(truncated.ok());
    EXPECT_EQ(truncated.status().code(), StatusCode::kCorruption);
}

// ---- Record types --------------------------------------------------------

TEST(AssertRecordTest, TheFiveTypesAreAssignedNamedAndClassified) {
    EXPECT_TRUE(wal::IsAssignedRecordType(18));
    EXPECT_TRUE(wal::IsAssignedRecordType(22));
    // **This used to assert that 23 was unassigned**, which was true when 22 was
    // the last type - and it is why `kHeapDeleteUnmark = 23` shipped unwritable:
    // the test agreed with the stale bound instead of with the enum. What it
    // meant to say is that the five assertion records are contiguous and
    // assigned, which is what it says now (record.hpp, and
    // WalRecordTest.EveryNamedTypeIsWritable is the general guard).
    for (std::uint8_t raw = 18; raw <= 22; ++raw) {
        EXPECT_TRUE(wal::IsAssignedRecordType(raw)) << "assertion record " << +raw;
    }

    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertReserve), "ASSERT_RESERVE");
    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertCommit), "ASSERT_COMMIT");
    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertRollback), "ASSERT_ROLLBACK");
    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertBuild), "ASSERT_BUILD");
    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertDrop), "ASSERT_DROP");
    // AS6a's sixth, appended past the original five (RC07).
    EXPECT_STREQ(wal::RecordTypeName(wal::RecordType::kAssertSnapshot), "ASSERT_SNAPSHOT");
    EXPECT_TRUE(wal::IsAssignedRecordType(
        static_cast<std::uint8_t>(wal::RecordType::kAssertSnapshot)));

    EXPECT_TRUE(IsAssertionRecord(wal::RecordType::kAssertReserve));
    EXPECT_TRUE(IsAssertionRecord(wal::RecordType::kAssertDrop));
    EXPECT_FALSE(IsAssertionRecord(wal::RecordType::kHeapInsert));
}

// ---- The fold ------------------------------------------------------------

// A live run and its replayed twin. The live side mutates through the
// ordinary APIs and records what it did; the replay side starts from a
// formatted page and an empty directory and folds the records.
class AssertionReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto live_page = live_store_.CreateAt(kEntryPage);
        ASSERT_TRUE(live_page.ok());
        ASSERT_TRUE(BoundCabinPage::Format(live_page.value().bytes()).ok());
        auto replay_page = replay_store_.CreateAt(kEntryPage);
        ASSERT_TRUE(replay_page.ok());
        ASSERT_TRUE(BoundCabinPage::Format(replay_page.value().bytes()).ok());
        context_.Adopt(kAssertionId, &rebuilt_);
    }

    // One entry through the live side - build or reservation - returning the
    // index it took, with the record appended to the log.
    std::uint16_t LiveEntry(wal::RecordType type, std::uint64_t txn_id, std::uint64_t pk,
                            std::int64_t value, std::int64_t group) {
        const std::uint8_t flags =
            type == wal::RecordType::kAssertReserve ? kEntryReserved : std::uint8_t{0};
        BoundCabinEntry entry = Entry(pk, value, flags);
        // The live writer's order (AS6a): the group's id is assigned and
        // stamped onto the entry *before* the page append, because the append
        // precedes the Apply that would otherwise create the group.
        entry.group_id = live_.EnsureGroupId(KeyOf(group));

        auto page = live_store_.Get(kEntryPage);
        EXPECT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        EXPECT_TRUE(view.ok());
        auto index = view.value().Append(entry);
        EXPECT_TRUE(index.ok());

        const std::string key = KeyOf(group);
        EXPECT_TRUE(live_.Apply(key, value, kEntryPage, index.value()).ok());
        log_.push_back(EntryRecord(type, txn_id, index.value(), entry, key));
        return index.value();
    }

    void LiveCommit(std::uint64_t txn_id, std::span<const std::uint16_t> indexes) {
        auto page = live_store_.Get(kEntryPage);
        ASSERT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        ASSERT_TRUE(view.ok());
        for (const std::uint16_t index : indexes) {
            ASSERT_TRUE(view.value().ClearReserved(index).ok());
        }
        log_.push_back(CommitRecord(txn_id, indexes));
    }

    void LiveRollback(std::uint64_t txn_id, std::int64_t delta, std::uint16_t index,
                      std::int64_t group) {
        const std::string key = KeyOf(group);
        ASSERT_TRUE(live_.Unapply(key, delta, kEntryPage, index).ok());
        // The live writer's order again: the directory drops the entry and the
        // page keeps the bytes with `kEntryOrphaned` set, which is what lets a
        // linkage rebuild reading only pages agree with the directory
        // (`AssertionEnforcer::AbortTxn`, `docs/spec/assertion.md` §7).
        auto page = live_store_.Get(kEntryPage);
        ASSERT_TRUE(page.ok());
        auto view = BoundCabinPage::Open(page.value().bytes());
        ASSERT_TRUE(view.ok());
        ASSERT_TRUE(view.value().MarkOrphaned(index).ok());
        log_.push_back(RollbackRecord(txn_id, delta, index, key));
    }

    InMemoryPageStore live_store_{200};
    InMemoryPageStore replay_store_{200};
    BoundCabin live_{BoundAggregate::kSum, /*bound=*/20};
    BoundCabin rebuilt_{BoundAggregate::kSum, /*bound=*/20};
    MapContext context_;
    std::vector<std::vector<std::byte>> log_;
};

TEST_F(AssertionReplayTest, TheFoldRestoresThePageAndTheDirectoryExactly) {
    // A build, two reservations, one commit, one abort - every record type
    // but DROP, in stream order.
    LiveEntry(wal::RecordType::kAssertBuild, wal::kNoTxnId, 1, 5, /*group=*/7);
    LiveEntry(wal::RecordType::kAssertBuild, wal::kNoTxnId, 2, 7, /*group=*/7);
    LiveEntry(wal::RecordType::kAssertBuild, wal::kNoTxnId, 3, 1, /*group=*/8);
    const std::uint16_t committed =
        LiveEntry(wal::RecordType::kAssertReserve, /*txn_id=*/7, 4, 3, /*group=*/8);
    const std::uint16_t aborted =
        LiveEntry(wal::RecordType::kAssertReserve, /*txn_id=*/9, 5, 2, /*group=*/7);
    const std::array<std::uint16_t, 1> commit_list = {committed};
    LiveCommit(/*txn_id=*/7, commit_list);
    LiveRollback(/*txn_id=*/9, /*delta=*/2, aborted, /*group=*/7);

    ASSERT_TRUE(Fold(log_, replay_store_, context_).ok());

    // §7's claim at its strongest: the page byte for byte. The aborted
    // entry's orphaned slot is part of the bytes, so this also pins that
    // replay orphans exactly what the live side orphaned.
    auto live_page = live_store_.Get(kEntryPage);
    auto replay_page = replay_store_.Get(kEntryPage);
    ASSERT_TRUE(live_page.ok());
    ASSERT_TRUE(replay_page.ok());
    EXPECT_EQ(std::memcmp(live_page.value().bytes().data(), replay_page.value().bytes().data(), kPageSize), 0);

    // The directory, header by header.
    for (const std::int64_t group : {std::int64_t{7}, std::int64_t{8}}) {
        const GroupHeader* was = live_.Find(KeyOf(group));
        const GroupHeader* now = rebuilt_.Find(KeyOf(group));
        ASSERT_NE(was, nullptr);
        ASSERT_NE(now, nullptr);
        EXPECT_EQ(now->count, was->count) << "group " << group;
        EXPECT_EQ(now->sum, was->sum) << "group " << group;
        EXPECT_EQ(now->entries, was->entries) << "group " << group;
    }
    EXPECT_EQ(rebuilt_.group_count(), live_.group_count());

    // header == Σ(entries) on the replayed store - §7's post-recovery
    // invariant, through the verification hook AST04 shipped.
    const BoundCabin::EntryReader reader = [this](PageId page_id, std::uint16_t index)
        -> StatusOr<BoundCabinEntry> {
        auto page = replay_store_.Get(page_id);
        if (!page.ok()) return page.status();
        auto view = BoundCabinPage::Open(page.value().bytes());
        if (!view.ok()) return view.status();
        return view.value().Read(index);
    };
    EXPECT_TRUE(rebuilt_.VerifyAgainstEntries(reader).ok());

    // The admission boundary answers identically, which is the "no gap
    // where a violating write could be admitted" half: group 7 holds 12 of
    // 20, so +8 is the last admissible delta and +9 must refuse - on both.
    for (const std::int64_t delta : {std::int64_t{8}, std::int64_t{9}}) {
        auto was = live_.Admit(KeyOf(7), delta);
        auto now = rebuilt_.Admit(KeyOf(7), delta);
        ASSERT_TRUE(was.ok());
        ASSERT_TRUE(now.ok());
        EXPECT_EQ(now.value().admitted, was.value().admitted) << "delta " << delta;
    }
}

TEST_F(AssertionReplayTest, ADepartureFoldsWithItsSignAndItsRollbackRestoresIt) {
    // AST07's flag: a departure entry contributes (-1, -value), and its
    // compensation - marked by the envelope's flag byte - restores with the
    // opposite sign. Build one row in, reserve its departure (a DELETE's
    // shape), roll the departure back: the group must end where it began.
    LiveEntry(wal::RecordType::kAssertBuild, wal::kNoTxnId, 1, 5, /*group=*/7);

    // The departure, live and recorded.
    BoundCabinEntry leaving =
        Entry(1, 5, static_cast<std::uint8_t>(kEntryReserved |
                                              storage::cabin::kEntryDeparture));
    // A departure's group must already exist, so its id is *read* rather than
    // ensured - the same asymmetry the live check path has (assertion_check.cpp).
    const GroupHeader* departing_from = live_.Find(KeyOf(7));
    ASSERT_NE(departing_from, nullptr);
    leaving.group_id = departing_from->group_id;
    auto page = live_store_.Get(kEntryPage);
    ASSERT_TRUE(page.ok());
    auto view = BoundCabinPage::Open(page.value().bytes());
    ASSERT_TRUE(view.ok());
    auto index = view.value().Append(leaving);
    ASSERT_TRUE(index.ok());
    ASSERT_TRUE(live_.ApplyDeparture(KeyOf(7), 5, kEntryPage, index.value()).ok());
    log_.push_back(EntryRecord(wal::RecordType::kAssertReserve, /*txn_id=*/9, index.value(),
                               leaving, KeyOf(7)));
    // Mid-stream truth: the group is empty while the departure is reserved.
    ASSERT_EQ(live_.Find(KeyOf(7))->count, 0);
    ASSERT_EQ(live_.Find(KeyOf(7))->sum, 0);

    // The abort, live and recorded with the flag.
    ASSERT_TRUE(live_.UnapplyDeparture(KeyOf(7), 5, kEntryPage, index.value()).ok());
    log_.push_back(RollbackRecord(/*txn_id=*/9, 5, index.value(), KeyOf(7),
                                  /*departure=*/true));

    ASSERT_TRUE(Fold(log_, replay_store_, context_).ok());
    const GroupHeader* was = live_.Find(KeyOf(7));
    const GroupHeader* now = rebuilt_.Find(KeyOf(7));
    ASSERT_NE(was, nullptr);
    ASSERT_NE(now, nullptr);
    EXPECT_EQ(now->count, was->count);
    EXPECT_EQ(now->count, 1);
    EXPECT_EQ(now->sum, was->sum);
    EXPECT_EQ(now->sum, 5);
    EXPECT_EQ(now->entries, was->entries);
}

TEST_F(AssertionReplayTest, AnUnknownAssertionIsSkippedWholePageHalfIncluded) {
    // The skip rule: after ASSERT_DROP the cabin's pages are freed and may
    // be reused, so a record for an unknown id must not touch its page.
    log_.push_back(EntryRecord(wal::RecordType::kAssertReserve, /*txn_id=*/7, 0,
                               Entry(1, 1, kEntryReserved), KeyOf(1)));

    MapContext empty;
    ASSERT_TRUE(Fold(log_, replay_store_, empty).ok());

    auto page = replay_store_.Get(kEntryPage);
    ASSERT_TRUE(page.ok());
    auto view = BoundCabinPage::Open(page.value().bytes());
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.value().entry_count(), 0) << "the page half must be skipped too";
}

TEST_F(AssertionReplayTest, DropForgetsTheDirectory) {
    log_.push_back(DropRecord(kAssertionId));
    ASSERT_TRUE(Fold(log_, replay_store_, context_).ok());
    EXPECT_FALSE(context_.Holds(kAssertionId));
    EXPECT_EQ(context_.drops(), 1);
}

TEST_F(AssertionReplayTest, AReserveWhoseEntryIsNotReservedIsCorruption) {
    // The one bit RESERVE and BUILD disagree on, checked so the two cannot
    // be confused on disk - in either direction.
    log_.push_back(EntryRecord(wal::RecordType::kAssertReserve, /*txn_id=*/7, 0,
                               Entry(1, 1, /*flags=*/0), KeyOf(1)));
    Status s = Fold(log_, replay_store_, context_);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);

    log_.clear();
    log_.push_back(EntryRecord(wal::RecordType::kAssertBuild, wal::kNoTxnId, 0,
                               Entry(1, 1, kEntryReserved), KeyOf(1)));
    s = Fold(log_, replay_store_, context_);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

TEST_F(AssertionReplayTest, AnEntryIndexPastTheAppendPointIsCorruption) {
    // Index 1 into an empty page: a hole. Stream order would have written
    // index 0 first, so this record cannot be applied and must say so.
    log_.push_back(EntryRecord(wal::RecordType::kAssertBuild, wal::kNoTxnId, 1,
                               Entry(1, 1, 0), KeyOf(1)));
    Status s = Fold(log_, replay_store_, context_);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption);
    EXPECT_NE(s.message().find("append point"), std::string::npos) << s.message();
}

TEST_F(AssertionReplayTest, ANonAssertionRecordIsRefused) {
    log_.push_back(WholeRecord(wal::RecordType::kHeapInsert, /*txn_id=*/7, kEntryPage, {}));
    Status s = Fold(log_, replay_store_, context_);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::exec
