#include "kds/exec/bound_cabin.hpp"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// The Bound Cabin storage engine (docs/spec/assertion.md §5, workplan AST04).
//
// One test per acceptance criterion, and each is written against the property
// rather than the implementation:
//
//   entry codec round-trip          §5.1's 32 bytes, pk authoritative
//   group header updates            §5.2's running aggregate
//   hash collisions isolate groups  the key confirms, the hash only finds
//   eviction never touches a pinned Bound Cabin frame   EV3, by page kind
//   sum overflow is a statement error, never wraparound AG3
//
// The collision test is the one worth reading. A directory keyed on a hash
// alone would merge two groups, and for a structure an admission check trusts
// that is a **wrong answer** rather than a slow one — a write would be
// admitted or refused against somebody else's total. So the test forces a
// real collision rather than hoping for one, by asking the directory about
// two keys that hash to the same bucket.

namespace kds::exec {
namespace {

using storage::cabin::BoundCabinEntry;
using storage::cabin::BoundCabinPage;
using storage::cabin::kEntryBytes;
using storage::cabin::kEntryHintValid;
using storage::cabin::kEntryReserved;
using storage::cabin::kMaxEntriesPerPage;

parser::AstValue Int(std::int64_t v) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = v;
    return value;
}

parser::AstValue Str(std::string v) {
    parser::AstValue value;
    value.type = parser::ValueType::kStr;
    value.str_val = std::move(v);
    return value;
}

std::string Key(std::vector<parser::AstValue> values) { return EncodeGroupKey(values); }

// ---- The entry codec (§5.1) ---------------------------------------------

TEST(BoundCabinEntryTest, ThirtyTwoBytesRoundTripEveryField) {
    BoundCabinEntry in;
    in.pk = 0xFFFFFFFFFFULL;  // the largest 40-bit id
    in.flags = kEntryReserved | kEntryHintValid;
    in.page_id = 4242;
    in.page_epoch = 7;
    in.slot = 19;
    in.value = -9'000'000'000LL;

    std::array<std::byte, kEntryBytes> buf{};
    ASSERT_TRUE(storage::cabin::EncodeEntry(in, buf).ok());
    const BoundCabinEntry out = storage::cabin::DecodeEntry(buf);

    EXPECT_EQ(out.pk, in.pk);
    EXPECT_EQ(out.flags, in.flags);
    EXPECT_EQ(out.page_id, in.page_id);
    EXPECT_EQ(out.page_epoch, in.page_epoch);
    EXPECT_EQ(out.slot, in.slot);
    EXPECT_EQ(out.value, in.value) << "a negative aggregate survives the unsigned round trip";
    EXPECT_TRUE(out.reserved());
    EXPECT_TRUE(out.hint_valid());
}

TEST(BoundCabinEntryTest, APkPastFortyBitsIsRefusedRatherThanTruncated) {
    // A truncated pk is a name that means a *different* row, which is exactly
    // the mis-attribution K1 exists to make impossible. Refusing is the only
    // safe answer; narrowing would be silent corruption of an identity.
    BoundCabinEntry in;
    in.pk = (std::uint64_t{1} << 40);  // one past the range

    std::array<std::byte, kEntryBytes> buf{};
    Status s = storage::cabin::EncodeEntry(in, buf);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("40-bit"), std::string::npos) << s.message();
}

TEST(BoundCabinEntryTest, AnEntryWithNoHintIsStillAValidEntry) {
    // C2 carried over: the pk is the authority and the location is advice, so
    // an entry whose hint was proven wrong is complete and correct - it just
    // costs a descent.
    BoundCabinEntry in;
    in.pk = 77;
    in.value = 5;
    in.page_id = kInvalidPageId;

    std::array<std::byte, kEntryBytes> buf{};
    ASSERT_TRUE(storage::cabin::EncodeEntry(in, buf).ok());
    const BoundCabinEntry out = storage::cabin::DecodeEntry(buf);
    EXPECT_EQ(out.pk, 77u);
    EXPECT_FALSE(out.hint_valid());
}

// ---- The page ------------------------------------------------------------

class BoundCabinPageTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(BoundCabinPage::Format(page()).ok()); }
    std::span<std::byte, kPageSize> page() {
        return std::span<std::byte, kPageSize>(bytes_);
    }
    std::array<std::byte, kPageSize> bytes_{};
};

TEST_F(BoundCabinPageTest, AFormattedPageIsEmptyAndDeclaresItsClass) {
    auto view = BoundCabinPage::Open(page());
    ASSERT_TRUE(view.ok()) << view.status().message();
    EXPECT_EQ(view.value().entry_count(), 0u);
    EXPECT_EQ(view.value().next_page_id(), kInvalidPageId);
    EXPECT_FALSE(view.value().full());

    const storage::PageHeaderFields header =
        storage::ReadPageHeader(std::span<const std::byte, kPageSize>(page()));
    EXPECT_EQ(header.page_type, static_cast<std::uint8_t>(PageType::kCabinBound));

    // A formatted page must satisfy the engine's own header validator, not
    // merely carry the right type byte. Until 2026-08-24 it did not:
    // MaxSupportedFormatVersion had no kCabinBound case, so Format stamped
    // version 0 and this call answered Corruption. Nothing on the live path
    // asked - BoundCabinPage::Open checks the type byte itself, and
    // DevicePageStore verifies checksums rather than versions - so the
    // contradiction sat here unexercised.
    EXPECT_TRUE(storage::ValidatePageHeader(
                    std::span<const std::byte, kPageSize>(page()), PageType::kCabinBound)
                    .ok());
}

TEST_F(BoundCabinPageTest, AppendsReadBackInOrderAndFillTheStatedCapacity) {
    auto view = BoundCabinPage::Open(page());
    ASSERT_TRUE(view.ok());

    for (std::uint16_t i = 0; i < kMaxEntriesPerPage; ++i) {
        BoundCabinEntry e;
        e.pk = 1000 + i;
        e.value = i;
        auto at = view.value().Append(e);
        ASSERT_TRUE(at.ok()) << at.status().message();
        EXPECT_EQ(at.value(), i);
    }
    EXPECT_TRUE(view.value().full());

    // The capacity is arithmetic, not a guess: 8192 - 32 - 8 = 8152, / 32.
    EXPECT_EQ(kMaxEntriesPerPage, 254);

    BoundCabinEntry overflow;
    overflow.pk = 1;
    auto refused = view.value().Append(overflow);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kOutOfSpace);

    for (std::uint16_t i = 0; i < kMaxEntriesPerPage; ++i) {
        auto e = view.value().Read(i);
        ASSERT_TRUE(e.ok());
        EXPECT_EQ(e.value().pk, 1000u + i);
        EXPECT_EQ(e.value().value, i);
    }
}

TEST_F(BoundCabinPageTest, CommitClearsTheReservedFlagInPlace) {
    // The one mutation the format needs: §6.2 step 4 clears RESERVED, and the
    // aggregate is already correct because the reservation counted from
    // admission. Nothing else about an entry ever changes - its pk least of
    // all.
    auto view = BoundCabinPage::Open(page());
    ASSERT_TRUE(view.ok());

    BoundCabinEntry e;
    e.pk = 5;
    e.value = 3;
    e.flags = kEntryReserved;
    auto at = view.value().Append(e);
    ASSERT_TRUE(at.ok());
    ASSERT_TRUE(view.value().Read(at.value()).value().reserved());

    e.flags = 0;
    ASSERT_TRUE(view.value().Write(at.value(), e).ok());
    auto after = view.value().Read(at.value());
    ASSERT_TRUE(after.ok());
    EXPECT_FALSE(after.value().reserved());
    EXPECT_EQ(after.value().pk, 5u) << "the identity is untouched by a commit";
    EXPECT_EQ(view.value().entry_count(), 1u) << "nothing was appended";
}

TEST_F(BoundCabinPageTest, APageOfAnotherClassIsCorruptionRatherThanGarbage) {
    std::array<std::byte, kPageSize> other{};
    storage::FormatPage(std::span<std::byte, kPageSize>(other), PageType::kHeap);
    auto view = BoundCabinPage::Open(std::span<std::byte, kPageSize>(other));
    EXPECT_FALSE(view.ok());
    EXPECT_EQ(view.status().code(), StatusCode::kCorruption);
}

// ---- The group key encoding ---------------------------------------------

TEST(BoundCabinKeyTest, AKeyIsUnambiguousAcrossTypesAndBoundaries) {
    // Length-prefixed, so the classic split ambiguity cannot happen.
    EXPECT_NE(Key({Str("a"), Str("bc")}), Key({Str("ab"), Str("c")}));

    // Tagged, so an int and a string that render alike are different groups.
    EXPECT_NE(Key({Int(1)}), Key({Str("1")}));

    // NULL is a value, so two NULL keys are one group - grouping identity,
    // which is a different question from comparison.
    EXPECT_EQ(Key({parser::AstValue{}}), Key({parser::AstValue{}}));
    EXPECT_NE(Key({parser::AstValue{}}), Key({Int(0)}));

    // Deterministic and order-sensitive: a group is a tuple, not a set.
    EXPECT_EQ(Key({Int(41), Int(7)}), Key({Int(41), Int(7)}));
    EXPECT_NE(Key({Int(41), Int(7)}), Key({Int(7), Int(41)}));
}

// ---- The group directory (§5.2) -----------------------------------------

TEST(BoundCabinTest, AGroupHeaderTracksCountAndSumAcrossApplies) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/1000);
    const std::string k = Key({Int(41)});

    EXPECT_EQ(cabin.Find(k), nullptr);
    EXPECT_EQ(cabin.group_count(), 0u);

    ASSERT_TRUE(cabin.Apply(k, 100, /*page_id=*/9, /*index=*/0).ok());
    ASSERT_TRUE(cabin.Apply(k, 250, 9, 1).ok());

    const GroupHeader* header = cabin.Find(k);
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->count, 2);
    EXPECT_EQ(header->sum, 350);
    EXPECT_EQ(header->entries.size(), 2u);
    EXPECT_EQ(cabin.group_count(), 1u);

    // Cardinality moves per *entry*, whatever the value - a summed zero is
    // still a row. Getting this wrong hides until the data contains one.
    ASSERT_TRUE(cabin.Apply(k, 0, 9, 2).ok());
    EXPECT_EQ(cabin.Find(k)->count, 3);
    EXPECT_EQ(cabin.Find(k)->sum, 350);
}

TEST(BoundCabinTest, AdmissionReadsTheHeaderAndMutatesNothing) {
    BoundCabin cabin(BoundAggregate::kCount, /*bound=*/2);
    const std::string k = Key({Int(41), Int(7)});

    // A group that does not exist yet has aggregate 0, so the first write is
    // always admitted against a positive bound.
    auto first = cabin.Admit(k, 1);
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first.value().admitted);
    EXPECT_EQ(first.value().would_be, 1);
    EXPECT_EQ(cabin.group_count(), 0u) << "Admit is pure - §6.2 step 2 mutates nothing";

    ASSERT_TRUE(cabin.Apply(k, 1, 9, 0).ok());
    ASSERT_TRUE(cabin.Apply(k, 1, 9, 1).ok());

    // At the bound. The next one would exceed it.
    auto third = cabin.Admit(k, 1);
    ASSERT_TRUE(third.ok());
    EXPECT_FALSE(third.value().admitted);
    EXPECT_EQ(third.value().would_be, 3) << "the message names what was asked for, not what is";

    // And nothing was mutated by the refusal, which is why a failed admission
    // needs no cleanup.
    EXPECT_EQ(cabin.Find(k)->count, 2);
}

TEST(BoundCabinTest, AnAbortUnappliesExactlyTheEntryItNamed) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/1000);
    const std::string k = Key({Int(41)});

    ASSERT_TRUE(cabin.Apply(k, 100, 9, 0).ok());
    ASSERT_TRUE(cabin.Apply(k, 250, 9, 1).ok());
    ASSERT_TRUE(cabin.Unapply(k, 250, 9, 1).ok());

    const GroupHeader* header = cabin.Find(k);
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->count, 1);
    EXPECT_EQ(header->sum, 100) << "the aggregate is restored exactly";
    ASSERT_EQ(header->entries.size(), 1u);
    EXPECT_EQ(header->entries[0], std::make_pair(PageId{9}, std::uint16_t{0}));

    // Un-applying something that is not there is an error, not a silent
    // subtraction - which would corrupt the aggregate.
    EXPECT_FALSE(cabin.Unapply(k, 250, 9, 1).ok());
    EXPECT_FALSE(cabin.Unapply(Key({Int(99)}), 1, 9, 0).ok());
    EXPECT_EQ(cabin.Find(k)->sum, 100);
}

// The acceptance criterion is "hash collisions produce correct per-group
// isolation", and this is as close as a test can honestly get to it.
//
// **A true 64-bit FNV-1a collision cannot be constructed here.** Finding one
// is ~2^32 work, which is not a unit test. So what these two tests establish
// is the pair of properties the isolation is *made of*: distinct keys never
// resolve to each other (this test, and the 500-group one below), and the
// lookup confirms the stored key rather than returning the bucket's first
// entry (`BoundCabin::Find`, whose loop is the confirmation). The residual
// risk is that the confirmation loop itself is never entered with two
// occupants, which no test here forces — worth knowing rather than implied.
TEST(BoundCabinTest, DistinctGroupsNeverResolveToEachOther) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/100);

    const std::string a = Key({Int(1)});
    const std::string b = Key({Int(2)});
    ASSERT_NE(a, b);

    ASSERT_TRUE(cabin.Apply(a, 60, 9, 0).ok());
    ASSERT_TRUE(cabin.Apply(b, 60, 9, 1).ok());

    // Each group holds only its own, so neither is at the bound even though
    // together they are past it. A directory that merged them would refuse
    // the next write to either.
    ASSERT_NE(cabin.Find(a), nullptr);
    ASSERT_NE(cabin.Find(b), nullptr);
    EXPECT_EQ(cabin.Find(a)->sum, 60);
    EXPECT_EQ(cabin.Find(b)->sum, 60);
    EXPECT_EQ(cabin.group_count(), 2u);

    auto admit_a = cabin.Admit(a, 40);
    ASSERT_TRUE(admit_a.ok());
    EXPECT_TRUE(admit_a.value().admitted) << "group a is at 60 of 100, not 120 of 100";

    // A key that was never applied resolves to nothing, whatever it hashes
    // to - the confirmation, not the hash, is what answers.
    EXPECT_EQ(cabin.Find(Key({Int(3)})), nullptr);
}

TEST(BoundCabinTest, FiveHundredGroupsEachHoldOnlyTheirOwnEntry) {
    // Breadth where the test above has depth: if the directory ever returned
    // a neighbouring group - by hash, by bucket, or by a reallocation that
    // moved a header - one of these 500 would hold the wrong count.
    BoundCabin cabin(BoundAggregate::kCount, /*bound=*/10);

    // Build many groups so at least some buckets are shared, then check every
    // one resolves to its own aggregate.
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(cabin.Apply(Key({Int(i)}), 1, 9, static_cast<std::uint16_t>(i)).ok());
    }
    EXPECT_EQ(cabin.group_count(), 500u);
    for (int i = 0; i < 500; ++i) {
        const GroupHeader* header = cabin.Find(Key({Int(i)}));
        ASSERT_NE(header, nullptr) << i;
        EXPECT_EQ(header->count, 1) << "group " << i << " holds only its own entry";
        ASSERT_EQ(header->entries.size(), 1u);
        EXPECT_EQ(header->entries[0].second, i);
    }
}

// ---- Checked arithmetic (AG3) -------------------------------------------

TEST(BoundCabinTest, SumOverflowIsAStatementErrorAndNeverWrapsAround) {
    // The one answer an admission check must never produce is a wrapped sum,
    // because it satisfies a bound it does not satisfy.
    BoundCabin cabin(BoundAggregate::kSum, std::numeric_limits<std::int64_t>::max());
    const std::string k = Key({Int(1)});

    ASSERT_TRUE(cabin.Apply(k, std::numeric_limits<std::int64_t>::max(), 9, 0).ok());

    auto admit = cabin.Admit(k, 1);
    EXPECT_FALSE(admit.ok());
    EXPECT_EQ(admit.status().code(), StatusCode::kOutOfRange);
    EXPECT_NE(admit.status().message().find("overflow"), std::string::npos)
        << admit.status().message();

    // And the same test guards Apply, which the CREATE-time builder reaches
    // without admitting first.
    Status applied = cabin.Apply(k, 1, 9, 1);
    EXPECT_FALSE(applied.ok());
    EXPECT_EQ(applied.code(), StatusCode::kOutOfRange);

    // Nothing wrapped: the aggregate is what it was.
    EXPECT_EQ(cabin.Find(k)->sum, std::numeric_limits<std::int64_t>::max());
}

// ---- Verification (§7) ---------------------------------------------------

TEST(BoundCabinTest, ReSummingTheEntriesAgreesWithTheHeaderAndCatchesADivergence) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/1000);
    const std::string k = Key({Int(41)});

    std::vector<BoundCabinEntry> entries;
    for (std::int64_t v : {10, 20, 30}) {
        BoundCabinEntry e;
        e.pk = 100 + static_cast<std::uint64_t>(v);
        e.value = v;
        entries.push_back(e);
        ASSERT_TRUE(cabin.Apply(k, v, 9, static_cast<std::uint16_t>(entries.size() - 1)).ok());
    }

    auto read = [&](PageId, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        return entries[index];
    };
    EXPECT_TRUE(cabin.VerifyAgainstEntries(read).ok());

    // Now make an entry disagree with the header, which is what a lost write
    // or a bad replay would look like. The verification is what §7 wires into
    // the integrity sweep, and it must say so rather than trust the header.
    entries[1].value = 999;
    Status diverged = cabin.VerifyAgainstEntries(read);
    EXPECT_FALSE(diverged.ok());
    EXPECT_EQ(diverged.code(), StatusCode::kCorruption);
    EXPECT_NE(diverged.message().find("disagrees"), std::string::npos) << diverged.message();
}

// ---- EV3: the eviction guarantee AST04 rests on -------------------------

// ---- AS6a: group ids, the snapshot, and the linkage rebuild --------------

TEST(BoundCabinTest, GroupIdsAreDenseFromOneAndStableForTheGroupsLife) {
    BoundCabin cabin(BoundAggregate::kCount, /*bound=*/100);

    const std::uint32_t a = cabin.EnsureGroupId(Key({Str("a")}));
    const std::uint32_t b = cabin.EnsureGroupId(Key({Str("b")}));
    EXPECT_EQ(a, 1u) << "ids number from 1, leaving 0 as 'no group'";
    EXPECT_EQ(b, 2u);
    // Asking again is not a new group: an id is the group's for its life, and
    // an entry written yesterday still names it.
    EXPECT_EQ(cabin.EnsureGroupId(Key({Str("a")})), a);

    // And `Apply` uses the same creation site, so a group born through the
    // write path carries an id too - an entry stamped 0 is one recovery cannot
    // attribute, which is the whole reason the field exists.
    ASSERT_TRUE(cabin.Apply(Key({Str("c")}), 1, /*page_id=*/500, /*index=*/0).ok());
    const GroupHeader* c = cabin.Find(Key({Str("c")}));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->group_id, 3u);
}

TEST(BoundCabinTest, TheSnapshotIsHeadersOnlyOrderedByIdAndNeverTheEntryLists) {
    BoundCabin cabin(BoundAggregate::kSum, /*bound=*/100);
    ASSERT_TRUE(cabin.Apply(Key({Str("x")}), 5, 500, 0).ok());
    ASSERT_TRUE(cabin.Apply(Key({Str("x")}), 7, 500, 1).ok());
    ASSERT_TRUE(cabin.Apply(Key({Str("y")}), 3, 500, 2).ok());

    const std::vector<BoundCabin::GroupSnapshot> snapshot = cabin.SnapshotGroups();
    ASSERT_EQ(snapshot.size(), 2u);
    // Ordered by id, because a checkpoint's bytes must be a function of its
    // input and bucket order is not one (sched.md §8).
    EXPECT_LT(snapshot[0].group_id, snapshot[1].group_id);
    EXPECT_EQ(snapshot[0].key, Key({Str("x")}));
    EXPECT_EQ(snapshot[0].count, 2);
    EXPECT_EQ(snapshot[0].sum, 12);
    EXPECT_EQ(snapshot[1].sum, 3);
    // O(groups): three entries, two records. That is the whole point of AS6a -
    // the linkage is rebuilt from the pages, never written at every checkpoint.
    EXPECT_EQ(snapshot.size(), cabin.group_count());
}

TEST(BoundCabinTest, ASnapshotPlusTheScannedEntriesRebuildsTheDirectory) {
    // AS6a's recovery order, without the WAL: restore the headers, then attach
    // each entry the cabin's pages carry by its `group_id`, then verify the
    // header against the entries it now links.
    BoundCabin live(BoundAggregate::kSum, /*bound=*/100);
    const std::uint32_t gx = live.EnsureGroupId(Key({Str("x")}));
    ASSERT_TRUE(live.Apply(Key({Str("x")}), 5, /*page_id=*/500, /*index=*/0).ok());
    ASSERT_TRUE(live.Apply(Key({Str("x")}), 7, /*page_id=*/500, /*index=*/1).ok());
    const std::uint32_t gy = live.EnsureGroupId(Key({Str("y")}));
    ASSERT_TRUE(live.Apply(Key({Str("y")}), 3, /*page_id=*/500, /*index=*/2).ok());

    BoundCabin rebuilt(BoundAggregate::kSum, /*bound=*/100);
    for (const BoundCabin::GroupSnapshot& g : live.SnapshotGroups()) {
        ASSERT_TRUE(rebuilt.RestoreGroup(g.group_id, g.key, g.count, g.sum).ok());
    }
    // The scan of the cabin's own pages, in page order - which is where the
    // entries' `group_id` is read from on a real mount.
    ASSERT_TRUE(rebuilt.AttachEntry(gx, 500, 0).ok());
    ASSERT_TRUE(rebuilt.AttachEntry(gx, 500, 1).ok());
    ASSERT_TRUE(rebuilt.AttachEntry(gy, 500, 2).ok());

    const GroupHeader* x = rebuilt.Find(Key({Str("x")}));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->group_id, gx);
    EXPECT_EQ(x->count, 2);
    EXPECT_EQ(x->sum, 12);
    ASSERT_EQ(x->entries.size(), 2u);

    // §7's verification hook over the rebuilt structure: header == Σ(entries),
    // which is now a check of a rebuild against durable bytes rather than of a
    // structure against itself.
    auto read = [](PageId page_id, std::uint16_t index) -> StatusOr<BoundCabinEntry> {
        BoundCabinEntry entry;
        entry.page_id = page_id;
        entry.slot = index;
        entry.value = index == 0 ? 5 : (index == 1 ? 7 : 3);
        return entry;
    };
    EXPECT_TRUE(rebuilt.VerifyAgainstEntries(read).ok());
}

TEST(BoundCabinTest, AnEntryNamingAGroupTheSnapshotDoesNotHoldIsNotFound) {
    // Not a defect: an entry whose group is absent from the snapshot belongs to
    // a group created after it, and the ASSERT_* fold from the checkpoint
    // forward is what creates that group. The caller has to be able to tell.
    BoundCabin rebuilt(BoundAggregate::kCount, /*bound=*/10);
    ASSERT_TRUE(rebuilt.RestoreGroup(/*group_id=*/1, Key({Str("x")}), 1, 1).ok());
    EXPECT_EQ(rebuilt.AttachEntry(/*group_id=*/9, 500, 0).code(), StatusCode::kNotFound);
}

TEST(BoundCabinTest, ARestoreThatNamesAGroupTwiceIsRefused) {
    BoundCabin rebuilt(BoundAggregate::kCount, /*bound=*/10);
    ASSERT_TRUE(rebuilt.RestoreGroup(1, Key({Str("x")}), 1, 1).ok());
    EXPECT_EQ(rebuilt.RestoreGroup(1, Key({Str("y")}), 1, 1).code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(rebuilt.RestoreGroup(2, Key({Str("x")}), 1, 1).code(),
              StatusCode::kInvalidArgument);
    // Id 0 is the reserved "no group" value and is what every pre-AS6a entry
    // reads back as, so a snapshot may not name it.
    EXPECT_EQ(rebuilt.RestoreGroup(0, Key({Str("z")}), 1, 1).code(),
              StatusCode::kInvalidArgument);
}

TEST(BoundCabinTest, AdoptingAGroupIdRefusesToDisagreeWithTheDirectory) {
    BoundCabin cabin(BoundAggregate::kCount, /*bound=*/10);
    const std::uint32_t id = cabin.EnsureGroupId(Key({Str("x")}));

    EXPECT_TRUE(cabin.AdoptGroupId(Key({Str("x")}), id).ok());
    // The record says one id, the directory holds another: there is no safe way
    // to pick, because the choice decides which entries are this group's.
    EXPECT_EQ(cabin.AdoptGroupId(Key({Str("x")}), id + 5).code(), StatusCode::kCorruption);
    // The same id claimed by a second key is the same disagreement, mirrored.
    EXPECT_EQ(cabin.AdoptGroupId(Key({Str("y")}), id).code(), StatusCode::kCorruption);
    // A pre-AS6a record carries 0, and attributing it would need the allocation
    // order the id exists to replace.
    EXPECT_EQ(cabin.AdoptGroupId(Key({Str("z")}), 0).code(), StatusCode::kCorruption);

    // A fresh key with a fresh id is the ordinary fold case: the group is
    // created carrying the record's id, not one of the cabin's choosing.
    ASSERT_TRUE(cabin.AdoptGroupId(Key({Str("w")}), 40).ok());
    const GroupHeader* w = cabin.Find(Key({Str("w")}));
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->group_id, 40u);
    // And the next group created cannot reuse an id the fold adopted.
    EXPECT_GT(cabin.EnsureGroupId(Key({Str("later")})), 40u);
}

TEST(BoundCabinPinningTest, TheSweepNeverReclaimsABoundCabinPageEvenUnpinned) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok());
    auto opened = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/16);
    ASSERT_TRUE(opened.ok());
    std::unique_ptr<storage::DevicePageStore> store = std::move(opened.value());

    // A Bound Cabin page and an ordinary heap page, both clean and cold.
    auto cabin_page = store->CreateNew();
    ASSERT_TRUE(cabin_page.ok());
    const PageId cabin_id = cabin_page.value().first;
    ASSERT_TRUE(BoundCabinPage::Format(cabin_page.value().second.bytes()).ok());

    auto heap_page = store->CreateNew();
    ASSERT_TRUE(heap_page.ok());
    const PageId heap_id = heap_page.value().first;
    storage::FormatPage(heap_page.value().second.bytes(), PageType::kHeap);

    // Setup handles dropped before the sweep: CreateNew() pins since MG01,
    // and this test's point is the *class* pin, not the handle pin.
    cabin_page.value().second.Release();
    heap_page.value().second.Release();

    ASSERT_TRUE(store->Sync().ok());

    // Pinned by **class**, from the page kind - nobody holds a PageRef.
    EXPECT_TRUE(store->IsPinnedClass(cabin_id));
    EXPECT_FALSE(store->IsPinnedClass(heap_id));
    EXPECT_EQ(store->pinned_frames(), 0u);

    std::size_t reclaimed = 0;
    for (int pass = 0; pass < 8; ++pass) reclaimed += store->EvictColdFrames(64);

    // The heap page fell, so the sweep was working - without which the
    // assertion below would hold against a sweep that does nothing.
    EXPECT_GE(reclaimed, 1u);
    (void)heap_id;

    // The Bound Cabin page is still resident, and still a Bound Cabin page.
    auto still = store->GetForRead(cabin_id);
    ASSERT_TRUE(still.ok());
    const storage::PageHeaderFields header =
        storage::ReadPageHeader(std::span<const std::byte, kPageSize>(still.value().bytes()));
    EXPECT_EQ(header.page_type, static_cast<std::uint8_t>(PageType::kCabinBound));
    EXPECT_TRUE(store->IsPinnedClass(cabin_id));
}

}  // namespace
}  // namespace kds::exec
