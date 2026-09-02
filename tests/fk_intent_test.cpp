#include "kds/server/fk_intent.hpp"

#include <gtest/gtest.h>

// SA-T4 — the row-scoped reference intent (`fk_intent.hpp`).
//
// The table is small enough that the interesting cases are all about its
// *rules* rather than its storage: who an intent excludes, what releases
// it, and what an idempotent add has to mean when a statement references
// one parent from several rows.

namespace kds::server {
namespace {

FkIntentHolder Holder(std::uint32_t core, std::uint64_t session) {
    return FkIntentHolder{core, session};
}

TEST(FkIntentTest, AForeignHolderBlocksAndTheOwnerDoesNot) {
    FkIntentTable table;
    const FkIntentHolder theirs = Holder(1, 10);
    const FkIntentHolder mine = Holder(0, 20);

    table.Add(/*parent_oid=*/900, /*parent_pk=*/7, theirs);

    // The whole of what the parent's owner learns: someone else is relying
    // on row 7.
    EXPECT_TRUE(table.HeldByAnotherThan(900, 7, mine));

    // And the exclusion, which is not a courtesy: a transaction that met
    // only its own reliance would retry forever on a state it created.
    EXPECT_FALSE(table.HeldByAnotherThan(900, 7, theirs));

    // Neither a different row nor a different relation is this intent's
    // business - it is row-scoped, which is the whole point of the name.
    EXPECT_FALSE(table.HeldByAnotherThan(900, 8, mine));
    EXPECT_FALSE(table.HeldByAnotherThan(901, 7, mine));
}

TEST(FkIntentTest, TheHolderIsBothFieldsBecauseNeitherIsUniqueAlone) {
    FkIntentTable table;
    table.Add(900, 7, Holder(/*core=*/1, /*session=*/10));

    // Same session id, different coordinator: a session id is unique only
    // on the core that minted it.
    EXPECT_TRUE(table.HeldByAnotherThan(900, 7, Holder(2, 10)));
    // Same coordinator, different session.
    EXPECT_TRUE(table.HeldByAnotherThan(900, 7, Holder(1, 11)));
    // Both equal: this is the holder, and the pair is
    // `ShippedStatementExecutor::DedupKey` - the same key the participant's
    // transaction context already uses, so a shipped DELETE can ask this
    // question with what it knows.
    EXPECT_FALSE(table.HeldByAnotherThan(900, 7, Holder(1, 10)));
}

TEST(FkIntentTest, AddIsIdempotentSoOneParentReferencedTwiceIsOneReliance) {
    FkIntentTable table;
    const FkIntentHolder holder = Holder(1, 10);

    // A multi-row INSERT whose rows all name parent 7, and then a retry.
    table.Add(900, 7, holder);
    table.Add(900, 7, holder);
    table.Add(900, 7, holder);

    EXPECT_EQ(table.live_rows(), 1u);
    // The counter is adds, not rows: it prices what the probe path did.
    EXPECT_EQ(table.stats().recorded, 3u);

    // And one release frees it - a set, so there is no reference count to
    // get wrong.
    EXPECT_EQ(table.Release(holder), 1u);
    EXPECT_EQ(table.live_rows(), 0u);
}

TEST(FkIntentTest, ReleaseTakesEveryRowThisHolderTookAndNobodyElsesRow) {
    FkIntentTable table;
    const FkIntentHolder a = Holder(1, 10);
    const FkIntentHolder b = Holder(2, 20);

    table.Add(900, 7, a);
    table.Add(900, 8, a);
    table.Add(901, 7, a);
    table.Add(900, 7, b);  // two holders on one row
    ASSERT_EQ(table.live_rows(), 3u);

    EXPECT_EQ(table.Release(a), 3u);
    // Row (900, 7) survives because b still relies on it; the other two
    // rows are gone entirely.
    EXPECT_EQ(table.live_rows(), 1u);
    EXPECT_TRUE(table.HeldByAnotherThan(900, 7, a));
    EXPECT_FALSE(table.HeldByAnotherThan(900, 8, a));

    EXPECT_EQ(table.stats().released, 3u);
}

TEST(FkIntentTest, ReleaseIsIdempotentForAResentDecide) {
    FkIntentTable table;
    const FkIntentHolder holder = Holder(1, 10);
    table.Add(900, 7, holder);

    EXPECT_EQ(table.Release(holder), 1u);
    // A decide resent after the ack was lost: a benign no-op, the same
    // reading `TxnDecideRequestPayload::retry` gives the prepared state it
    // travels with.
    EXPECT_EQ(table.Release(holder), 0u);
    EXPECT_EQ(table.stats().released, 1u);
}

TEST(FkIntentTest, AnEmptyTableHoldsNothingAgainstAnybody) {
    const FkIntentTable table;
    EXPECT_EQ(table.live_rows(), 0u);
    EXPECT_FALSE(table.HeldByAnotherThan(900, 7, Holder(1, 10)));
    EXPECT_EQ(table.stats().recorded, 0u);
}

// ---- AJ-T1: the pending-delete set ---------------------------------------
//
// The mirror above, and the same kind of file: what is interesting is the
// rules, not the storage.

TEST(FkPendingDeleteTest, ARegisteredRowIsPending) {
    FkPendingDeleteTable table;
    table.Add(/*parent_oid=*/900, /*parent_pk=*/7, /*session_id=*/3);

    // **The registrant's own id does not exempt it**, and that is the
    // design rather than an omission: the asker's session id comes from
    // another core's counter and this table's from ours, both minted from
    // 1, so a predicate that compared them would answer "not pending" on a
    // collision - vouching for a row on its way out. The header carries the
    // argument; here it is the reason the row is pending for a probe that
    // happens to carry id 3 too.
    EXPECT_TRUE(table.Pending(900, 7));
    // Keyed on the row, not the relation.
    EXPECT_FALSE(table.Pending(900, 8));
    EXPECT_FALSE(table.Pending(901, 7));
    EXPECT_EQ(table.live_rows(), 1u);
}

TEST(FkPendingDeleteTest, TwoSessionsOnOneRowAndOneDecidingDoesNotFreeTheOther) {
    // The reason the value is a set: two DELETEs of the same row can be
    // outstanding at once across a park, and the first to decide must not
    // reopen the window the second is still relying on.
    FkPendingDeleteTable table;
    table.Add(900, 7, /*session_id=*/3);
    table.Add(900, 7, /*session_id=*/4);
    EXPECT_EQ(table.live_rows(), 1u);

    EXPECT_EQ(table.Release(/*session_id=*/3), 1u);
    EXPECT_EQ(table.live_rows(), 1u) << "session 4's registration was freed by session 3's clear";
    EXPECT_TRUE(table.Pending(900, 7));

    EXPECT_EQ(table.Release(/*session_id=*/4), 1u);
    EXPECT_EQ(table.live_rows(), 0u);
}

TEST(FkPendingDeleteTest, AddIsIdempotentAndReleaseIsToo) {
    FkPendingDeleteTable table;
    // A DELETE that resumes after its park runs the whole statement again,
    // registration included, so a second add of the same row by the same
    // session is normal operation rather than a fault.
    table.Add(900, 7, /*session_id=*/3);
    table.Add(900, 7, /*session_id=*/3);
    EXPECT_EQ(table.live_rows(), 1u);
    EXPECT_EQ(table.stats().recorded, 2u) << "adds are counted, rows are not";

    EXPECT_EQ(table.Release(/*session_id=*/3), 1u);
    // **Twice**, because the clear runs at the end of every write statement
    // and again when the transaction ends; the second must be free.
    EXPECT_EQ(table.Release(/*session_id=*/3), 0u);
    EXPECT_EQ(table.stats().released, 1u);
}

TEST(FkPendingDeleteTest, ASessionsWholeRegistrationEndsAtOnce) {
    FkPendingDeleteTable table;
    table.Add(900, 7, /*session_id=*/3);
    table.Add(900, 8, /*session_id=*/3);
    table.Add(901, 7, /*session_id=*/3);
    EXPECT_EQ(table.live_rows(), 3u);

    EXPECT_EQ(table.Release(/*session_id=*/3), 3u);
    EXPECT_EQ(table.live_rows(), 0u);
}

TEST(FkPendingDeleteTest, AnEmptyTableHoldsNothingAgainstAnybody) {
    const FkPendingDeleteTable table;
    EXPECT_EQ(table.live_rows(), 0u);
    EXPECT_FALSE(table.Pending(900, 7));
    EXPECT_EQ(table.stats().recorded, 0u);
}

}  // namespace
}  // namespace kds::server
