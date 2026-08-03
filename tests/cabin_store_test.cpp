#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kds/catalog/rows.hpp"
#include "kds/stats/cabin_store.hpp"

// The Cabin runtime store in isolation (docs/cabin-workplan.md CB04): the
// n=2/n=1 policy, the append-only maintenance rule, and the two behaviours
// that are correctness rather than policy - a cap **refuses to observe**
// instead of truncating a set, and un-observing is always available.

namespace kds::stats {
namespace {

parser::AstValue Str(const char* text) {
    parser::AstValue value;
    value.type = parser::ValueType::kStr;
    value.str_val = text;
    return value;
}

parser::AstValue Int(std::int64_t v) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = v;
    return value;
}

CabinEntry EntryFor(std::uint64_t pk, PageId page = 100, std::uint16_t slot = 0) {
    CabinEntry entry;
    entry.pk = pk;
    entry.page_id = page;
    entry.slot = slot;
    entry.flags = kCabinHintValid;
    return entry;
}

CabinKey KeyFor(std::uint64_t cabin_id, const parser::AstValue& value) {
    auto key = MakeCabinKey(cabin_id, value);
    EXPECT_TRUE(key.has_value());
    return *key;
}

TEST(CabinStoreTest, EntryIsTwentyFourBytes) {
    // C6 fixes it, and the whole of §8's full-coverage arithmetic rests on
    // the number.
    EXPECT_EQ(sizeof(CabinEntry), 24u);
}

TEST(CabinStoreTest, ValuesThatCanNeverBeObservedHaveNoKey) {
    parser::AstValue null_value;  // kNull by default
    EXPECT_FALSE(MakeCabinKey(1, null_value).has_value());

    parser::AstValue param;
    param.type = parser::ValueType::kParam;
    param.str_val = "x";
    EXPECT_FALSE(MakeCabinKey(1, param).has_value());

    // And a cabin_id of 0 is not a cabin.
    EXPECT_FALSE(MakeCabinKey(0, Str("aaa")).has_value());
}

TEST(CabinStoreTest, DistinctValuesNeverShareASet) {
    // The reason CabinKey holds the value rather than a hash of it: a
    // collision here would serve one value's rows for another's, where
    // Waystone's arg_hash can afford one because it costs only a replay.
    CabinStore store;
    const CabinKey a = KeyFor(1, Str("aaa"));
    const CabinKey b = KeyFor(1, Str("bbb"));
    const CabinKey same_text_other_cabin = KeyFor(2, Str("aaa"));
    const CabinKey int_one = KeyFor(1, Int(1));

    EXPECT_TRUE(store.Commit(a, {EntryFor(1)}));
    EXPECT_NE(store.Find(a), nullptr);
    EXPECT_EQ(store.Find(b), nullptr);
    EXPECT_EQ(store.Find(same_text_other_cabin), nullptr);
    EXPECT_EQ(store.Find(int_one), nullptr);
}

TEST(CabinStoreTest, AnObservedEmptySetIsNotTheSameAsUnobserved) {
    // The distinction the whole authority claim rests on: nullptr means
    // "scan, this Cabin knows nothing"; a non-null empty set means "no rows,
    // authoritatively".
    CabinStore store;
    const CabinKey key = KeyFor(1, Str("zzz"));
    EXPECT_EQ(store.Find(key), nullptr);

    EXPECT_TRUE(store.Commit(key, {}));
    std::vector<CabinEntry>* entries = store.Find(key);
    ASSERT_NE(entries, nullptr);
    EXPECT_TRUE(entries->empty());
}

TEST(CabinStoreTest, RecordsOnTheSecondSightingAndTheFirstWhenDeclared) {
    CabinStore store;
    const CabinKey key = KeyFor(1, Str("aaa"));

    const std::uint8_t first = store.Observe(key);
    EXPECT_EQ(first, 1);
    EXPECT_FALSE(store.WouldRecord(first, /*declared=*/false));
    // A declaration is the evidence n=2 waits for.
    EXPECT_TRUE(store.WouldRecord(first, /*declared=*/true));

    const std::uint8_t second = store.Observe(key);
    EXPECT_EQ(second, 2);
    EXPECT_TRUE(store.WouldRecord(second, /*declared=*/false));
}

TEST(CabinStoreTest, CommittingClearsTheSighting) {
    CabinStore store;
    const CabinKey key = KeyFor(1, Str("aaa"));
    store.Observe(key);
    store.Observe(key);
    ASSERT_TRUE(store.Commit(key, {EntryFor(1)}));

    // The value is observed, so its sighting count has no further use - and
    // the table it sat in is bounded by burst width.
    store.Unobserve(key);
    EXPECT_EQ(store.Observe(key), 1) << "an un-observed value re-earns its way back";
}

TEST(CabinStoreTest, SightingOverflowRestartsCountingRatherThanFailing) {
    CabinStore store;
    for (std::size_t i = 0; i <= CabinStore::kMaxSightings; ++i) {
        store.Observe(KeyFor(1, Int(static_cast<std::int64_t>(i))));
    }
    EXPECT_GE(store.stats().sighting_clears, 1u);
    // Nothing failed; a value merely lost a sighting, which costs one more
    // execution before it records. A performance event.
    EXPECT_EQ(store.Observe(KeyFor(1, Int(0))), 1);
}

TEST(CabinStoreTest, TheWriteHookAppendsOnlyToObservedValues) {
    CabinStore store;
    const CabinKey observed = KeyFor(1, Str("aaa"));
    const CabinKey unobserved = KeyFor(1, Str("bbb"));
    ASSERT_TRUE(store.Commit(observed, {EntryFor(1)}));

    store.NoteWrite(observed, EntryFor(2));
    store.NoteWrite(unobserved, EntryFor(3));

    ASSERT_NE(store.Find(observed), nullptr);
    EXPECT_EQ(store.Find(observed)->size(), 2u);
    // The common case, and the whole reason the hook is affordable: nothing
    // to invalidate, so nothing recorded.
    EXPECT_EQ(store.Find(unobserved), nullptr);
    EXPECT_EQ(store.stats().appends, 1u);
}

TEST(CabinStoreTest, AValueRoundTripDuplicatesAPkAndThatIsExpected) {
    // v -> v' -> v under append-only maintenance. The duplicate is not
    // damage: nothing may remove an entry on the hot path, because an older
    // snapshot may still match through it. The *read* dedupes.
    CabinStore store;
    const CabinKey v = KeyFor(1, Str("aaa"));
    ASSERT_TRUE(store.Commit(v, {EntryFor(1)}));

    store.NoteWrite(v, EntryFor(1));  // updated away and back
    ASSERT_NE(store.Find(v), nullptr);
    EXPECT_EQ(store.Find(v)->size(), 2u);
    EXPECT_EQ((*store.Find(v))[0].pk, (*store.Find(v))[1].pk);
}

TEST(CabinStoreTest, APerValueCapRefusesToObserveRatherThanTruncating) {
    // The rule that is correctness and not policy. A truncated set marked
    // observed is missing qualifying pks, which is the one thing the
    // invariant forbids outright.
    CabinLimits limits;
    limits.max_entries_per_value = 3;
    CabinStore store(limits);

    const CabinKey key = KeyFor(1, Str("aaa"));
    std::vector<CabinEntry> four = {EntryFor(1), EntryFor(2), EntryFor(3), EntryFor(4)};
    EXPECT_FALSE(store.Commit(key, four));
    EXPECT_EQ(store.Find(key), nullptr) << "the value must be unobserved, not partly observed";
    EXPECT_EQ(store.stats().cap_refusals, 1u);
}

TEST(CabinStoreTest, AnAppendPastTheCapUnobservesRatherThanDroppingTheAppend) {
    // The same rule from the other side. Dropping the append would leave a
    // set marked authoritative that is missing a row; un-observing costs a
    // scan, which is always legal.
    CabinLimits limits;
    limits.max_entries_per_value = 2;
    CabinStore store(limits);

    const CabinKey key = KeyFor(1, Str("aaa"));
    ASSERT_TRUE(store.Commit(key, {EntryFor(1), EntryFor(2)}));
    store.NoteWrite(key, EntryFor(3));

    EXPECT_EQ(store.Find(key), nullptr);
    EXPECT_EQ(store.stats().unobserved, 1u);
}

TEST(CabinStoreTest, APerCabinValueCapRefusesNewValuesAndKeepsTheOldOnes) {
    CabinLimits limits;
    limits.max_values = 2;
    CabinStore store(limits);

    const CabinKey a = KeyFor(1, Str("aaa"));
    const CabinKey b = KeyFor(1, Str("bbb"));
    const CabinKey c = KeyFor(1, Str("ccc"));
    ASSERT_TRUE(store.Commit(a, {EntryFor(1)}));
    ASSERT_TRUE(store.Commit(b, {EntryFor(2)}));
    EXPECT_FALSE(store.Commit(c, {EntryFor(3)}));

    // Refusing a new value never evicts an existing one: eviction is a
    // policy decision §8 has not made, and this refusal is undone by the
    // next execution.
    EXPECT_NE(store.Find(a), nullptr);
    EXPECT_NE(store.Find(b), nullptr);
    EXPECT_EQ(store.Find(c), nullptr);

    // A cap of zero observes nothing, which is the documented way to keep
    // the catalog objects and switch the behaviour off.
    CabinLimits none;
    none.max_values = 0;
    CabinStore off(none);
    EXPECT_FALSE(off.Commit(a, {EntryFor(1)}));
}

TEST(CabinStoreTest, RecommittingAnObservedValueReplacesItsSet) {
    // The heal path: a heap relation whose hint failed re-records the value
    // from a completed authoritative walk. Sound for the same reason the
    // first recording is.
    CabinStore store;
    const CabinKey key = KeyFor(1, Str("aaa"));
    ASSERT_TRUE(store.Commit(key, {EntryFor(1), EntryFor(2)}));
    ASSERT_TRUE(store.Commit(key, {EntryFor(3)}));

    ASSERT_NE(store.Find(key), nullptr);
    ASSERT_EQ(store.Find(key)->size(), 1u);
    EXPECT_EQ((*store.Find(key))[0].pk, 3u);
    EXPECT_EQ(store.InfoFor(1).values, 1u) << "replacing a set must not double-count it";
}

TEST(CabinStoreTest, ForgetDropsOneCabinAndLeavesTheOthers) {
    CabinStore store;
    const CabinKey mine = KeyFor(1, Str("aaa"));
    const CabinKey theirs = KeyFor(2, Str("aaa"));
    ASSERT_TRUE(store.Commit(mine, {EntryFor(1)}));
    ASSERT_TRUE(store.Commit(theirs, {EntryFor(1)}));

    store.Forget(1);
    EXPECT_EQ(store.Find(mine), nullptr);
    EXPECT_NE(store.Find(theirs), nullptr);
}

TEST(CabinStoreTest, InfoTracksValuesAndEntriesPerCabin) {
    CabinStore store;
    ASSERT_TRUE(store.Commit(KeyFor(1, Str("aaa")), {EntryFor(1), EntryFor(2)}));
    ASSERT_TRUE(store.Commit(KeyFor(1, Str("bbb")), {EntryFor(3)}));
    store.NoteWrite(KeyFor(1, Str("bbb")), EntryFor(4));
    store.NoteHit(1);
    store.NoteMiss(1);

    const CabinStore::CabinInfo info = store.InfoFor(1);
    EXPECT_EQ(info.values, 2u);
    EXPECT_EQ(info.entries, 4u);
    EXPECT_EQ(info.hits, 1u);
    EXPECT_EQ(info.misses, 1u);
    EXPECT_EQ(store.InfoFor(99).values, 0u) << "an unknown cabin reports zeros, not garbage";
}

// ---- The catalog row ------------------------------------------------------

TEST(CabinStoreTest, SysCabinRowRoundTrips) {
    catalog::SysCabinRow row{};
    row.cabin_id = 0x0102030405060708ULL;
    row.rel_oid = 4000;
    row.observed_ct = 12345;
    row.column_no = 7;
    row.origin = catalog::kCabinOriginUser;
    row.status = catalog::kCabinStatusActive;

    const auto encoded = row.Encode();
    auto decoded = catalog::SysCabinRow::Decode(encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().cabin_id, row.cabin_id);
    EXPECT_EQ(decoded.value().rel_oid, row.rel_oid);
    EXPECT_EQ(decoded.value().observed_ct, row.observed_ct);
    EXPECT_EQ(decoded.value().column_no, row.column_no);
    EXPECT_EQ(decoded.value().origin, row.origin);
    EXPECT_EQ(decoded.value().status, row.status);

    // A short buffer is corruption, not a partial row - the size check is
    // the only corruption signal a pure decode owns.
    std::vector<std::byte> short_buffer(catalog::SysCabinRow::kOnDiskSize - 1);
    EXPECT_FALSE(catalog::SysCabinRow::Decode(short_buffer).ok());
}

TEST(CabinStoreTest, AZeroedCabinRowNamesNothingReal) {
    // 0 is reserved in both `origin` and `status` for the reason
    // StoredStatementClass and StoredAccessKind each had to learn: a zeroed
    // page must not read as an active user-declared Cabin.
    std::vector<std::byte> zeros(catalog::SysCabinRow::kOnDiskSize);
    auto decoded = catalog::SysCabinRow::Decode(zeros);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().origin, catalog::kCabinOriginUnset);
    EXPECT_EQ(decoded.value().status, catalog::kCabinStatusUnset);
    EXPECT_FALSE(catalog::IsCabinServing(decoded.value()));
}

TEST(CabinStoreTest, AnUnsetColumnPolicyReadsAsAuto) {
    // Stored distinctly - "nothing was said" is not "the engine may decide"
    // - but every reader treats them alike, through one function.
    EXPECT_EQ(catalog::EffectiveCabinPolicy(catalog::kCabinPolicyUnset),
              catalog::kCabinPolicyAuto);
    EXPECT_TRUE(catalog::CabinPolicyPermitsCreation(catalog::kCabinPolicyUnset));
    EXPECT_TRUE(catalog::CabinPolicyPermitsCreation(catalog::kCabinPolicyAuto));
    EXPECT_TRUE(catalog::CabinPolicyPermitsCreation(catalog::kCabinPolicyEnabled));
    EXPECT_FALSE(catalog::CabinPolicyPermitsCreation(catalog::kCabinPolicyDisabled));
}

}  // namespace
}  // namespace kds::stats
