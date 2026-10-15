#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"

// BX-S1 (`instructions/v3.2.0/workorder-bx-unique-index.md`): the key unit
// and the latest-state check, below any SQL surface. The index under test is
// an ordinary one - `CREATE UNIQUE INDEX` arrives in BX-S2 - because the
// check reads only the tree and the rows, never the flag.
//
// Every row of BX-R1's table has a cell here, and each reaches its verdict
// through the real tree and the real clustered rows: the surplus entries an
// UPDATE, a DELETE and a ROLLBACK leave behind are what the check exists to
// see past.

namespace kds::exec {
namespace {

class IndexUniqueTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(const std::string& sql, server::Session* session = nullptr) {
        return dispatcher_->Dispatch(sql, session).response;
    }

    void Ok(const std::string& sql, server::Session* session = nullptr) {
        const std::string out = Run(sql, session);
        ASSERT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
    }

    const catalog::TableAccess& Access(const std::string& table) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        return *access.value();
    }

    // The key `value` would carry in the table's only index, on column `col`.
    UniqueKey Key(const catalog::TableAccess& access, std::uint16_t col, parser::AstValue value) {
        std::vector<parser::AstValue> row(access.schema.columns.size());
        row[col] = std::move(value);
        return KeyOf(access, access.indexes.front(), row);
    }

    // An INSERT's key: nothing to have moved from, so it is always there.
    static UniqueKey KeyOf(const catalog::TableAccess& access,
                           const catalog::TableAccess::IndexRef& ix,
                           const std::vector<parser::AstValue>& row) {
        auto key = EncodeUniqueKey(access, ix, row, /*first_col_pos=*/0);
        EXPECT_TRUE(key.ok()) << key.status().message();
        return key.ok() && key.value().has_value() ? *key.value() : UniqueKey{};
    }

    static parser::AstValue Int(std::int64_t v) {
        parser::AstValue out;
        out.type = parser::ValueType::kInt;
        out.int_val = v;
        return out;
    }

    static parser::AstValue Str(std::string v) {
        parser::AstValue out;
        out.type = parser::ValueType::kStr;
        out.str_val = std::move(v);
        return out;
    }

    // Asked as a writer that is no transaction: every other writer's
    // undecided row is busy to it.
    UniqueVerdict Check(const std::string& table, std::uint16_t col, parser::AstValue value,
                        std::uint64_t writer = 0, std::uint64_t self = 0) {
        const catalog::TableAccess& access = Access(table);
        auto verdict = CheckUniqueKey(store_, access, access.indexes.front(),
                                      Key(access, col, std::move(value)),
                                      mgr_->MintCheckView(writer), self);
        EXPECT_TRUE(verdict.ok()) << verdict.status().message();
        return verdict.ok() ? verdict.value() : UniqueVerdict{};
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

using Kind = UniqueVerdict::Kind;

TEST_F(IndexUniqueTest, AnEmptyRunIsFree) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, ALiveCommittedRowHoldsItsKey) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    const UniqueVerdict held = Check("t", 1, Int(7));
    EXPECT_EQ(held.kind, Kind::kHeld);
    EXPECT_EQ(held.tuple_key, 1u);
    EXPECT_EQ(Check("t", 1, Int(8)).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, TheRowBeingWrittenIsSkipped) {
    // An UPDATE's own row holds its old key; the check must not refuse the
    // row for colliding with itself.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    EXPECT_EQ(Check("t", 1, Int(7), /*writer=*/0, /*self=*/1).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, AKeyMovedOffLeavesASurplusEntryThatHoldsNothing) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    Ok("UPDATE t SET owner = 8 WHERE id = 1");
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
    EXPECT_EQ(Check("t", 1, Int(8)).kind, Kind::kHeld);
}

TEST_F(IndexUniqueTest, ADeletedRowHoldsNothing) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    Ok("DELETE FROM t WHERE id = 1");
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, ARolledBackInsertHoldsNothing) {
    // The rollback retires the slot and leaves the entry: the tuple key
    // resolves to nothing.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("INSERT INTO t VALUES (7)", &s);
    Ok("ROLLBACK", &s);
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, AnUndecidedWriterIsBusyToOthersAndHeldToItself) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("INSERT INTO t VALUES (7)", &s);

    const UniqueVerdict busy = Check("t", 1, Int(7));
    ASSERT_EQ(busy.kind, Kind::kBusy);
    EXPECT_EQ(busy.tuple_key, 1u);
    ASSERT_NE(busy.writer, 0u);

    // The writer's own check view sees its pending row.
    EXPECT_EQ(Check("t", 1, Int(7), /*writer=*/busy.writer).kind, Kind::kHeld);
    Ok("COMMIT", &s);
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kHeld);
}

TEST_F(IndexUniqueTest, AnUndecidedDeleteIsBusy) {
    // Its rollback would leave the key held, so the check cannot call it
    // free until the deleter decides.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    server::Session s;
    Ok("BEGIN", &s);
    Ok("DELETE FROM t WHERE id = 1", &s);
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kBusy);
    Ok("COMMIT", &s);
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, TwoStringsSharingTheEncodedPrefixAreDistinct) {
    // The index stores a 32-byte prefix (IX6), so both strings encode to one
    // key and land in one run; only the full-value comparison tells them apart.
    Ok("CREATE TABLE t (id int64, code varchar(64)) BTREE");
    Ok("CREATE INDEX by_code ON t (code)");
    const std::string prefix(32, 'p');
    Ok("INSERT INTO t VALUES ('" + prefix + "-first')");
    EXPECT_EQ(Check("t", 1, Str(prefix + "-second")).kind, Kind::kFree);
    EXPECT_EQ(Check("t", 1, Str(prefix + "-first")).kind, Kind::kHeld);
}

TEST_F(IndexUniqueTest, ACompositeKeyIsHeldOnlyWhole) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX by_ab ON t (a, b)");
    Ok("INSERT INTO t VALUES (1, 2)");
    const catalog::TableAccess& access = Access("t");
    std::vector<parser::AstValue> row(access.schema.columns.size());
    row[1] = Int(1);
    row[2] = Int(3);
    auto verdict = CheckUniqueKey(store_, access, access.indexes.front(),
                                  KeyOf(access, access.indexes.front(), row),
                                  mgr_->MintCheckView(0), 0);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value().kind, Kind::kFree);
    row[2] = Int(2);
    verdict = CheckUniqueKey(store_, access, access.indexes.front(),
                             KeyOf(access, access.indexes.front(), row), mgr_->MintCheckView(0),
                             0);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value().kind, Kind::kHeld);
}

TEST_F(IndexUniqueTest, OnlyAKeyColumnMovingIsAMove) {
    // BX-R5: a covered column moving appends an entry, and is not checked.
    Ok("CREATE TABLE t (id int64, owner int64, note int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner) COVERING (note)");
    const catalog::TableAccess& access = Access("t");
    const auto& ix = access.indexes.front();
    std::vector<parser::AstValue> before = {Int(1), Int(7), Int(100)};
    std::vector<parser::AstValue> covered = {Int(1), Int(7), Int(101)};
    std::vector<parser::AstValue> keyed = {Int(1), Int(8), Int(100)};
    EXPECT_FALSE(EncodeUniqueKey(access, ix, covered, 0, before).value().has_value());
    EXPECT_TRUE(EncodeUniqueKey(access, ix, keyed, 0, before).value().has_value());
    EXPECT_TRUE(EncodeUniqueKey(access, ix, keyed, 0).value().has_value())
        << "an INSERT always moves";
}

TEST_F(IndexUniqueTest, APurgedKeyHoldsNothingAndItsReuseHoldsOnlyItsOwnValue) {
    // `PURGE` retires the slot keyless and leaves the entry (BH-Q8 (a)); a
    // named INSERT of the purged tuple key then places a row with another
    // value under the same tuple key - the surplus entry resolves to it and
    // the full-value compare sees past it.
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    Ok("INSERT INTO t VALUES (7)");
    Ok("DELETE FROM t WHERE id = 1");
    Ok("PURGE FROM t WHERE id = 1");
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
    Ok("INSERT INTO t VALUES (1, 9)");
    EXPECT_EQ(Check("t", 1, Int(7)).kind, Kind::kFree);
    EXPECT_EQ(Check("t", 1, Int(9)).kind, Kind::kHeld);
}

TEST_F(IndexUniqueTest, ADateKeyComparesItsStorageFormAgainstTheLiteral) {
    // The written value is a literal coerced to the epoch day; the resolved
    // row decodes to the same storage form. A compare across the two forms
    // would never be equal and would admit every duplicate.
    Ok("CREATE TABLE t (id int64, d date) BTREE");
    Ok("CREATE INDEX by_d ON t (d)");
    Ok("INSERT INTO t VALUES ('2026-10-15')");
    EXPECT_EQ(Check("t", 1, Str("2026-10-15")).kind, Kind::kHeld);
    EXPECT_EQ(Check("t", 1, Str("2026-10-16")).kind, Kind::kFree);
}

TEST_F(IndexUniqueTest, TheViolationNamesIndexColumnsValueHolderAndByte) {
    Ok("CREATE TABLE t (id int64, owner int64) BTREE");
    Ok("CREATE INDEX by_owner ON t (owner)");
    const catalog::TableAccess& access = Access("t");
    const Status refused =
        UniqueViolation(access, access.indexes.front(), "by_owner", Key(access, 1, Int(7)),
                        /*holder=*/3, /*byte=*/29);
    EXPECT_EQ(refused.code(), StatusCode::kAlreadyExists);
    EXPECT_EQ(refused.message(),
              "unique index 'by_owner' on (owner): (7) is already held by tuple key 3 (byte 29)");
}

// ---- The key unit (BX-R3) ---------------------------------------------------

TEST_F(IndexUniqueTest, TheKeyUnitIsNamedByIndexAndValueUnderItsRelation) {
    Ok("CREATE TABLE t (id int64, a int64, b int64) BTREE");
    Ok("CREATE INDEX by_a ON t (a)");
    Ok("CREATE INDEX by_b ON t (b)");
    const catalog::TableAccess& access = Access("t");
    std::vector<parser::AstValue> row = {Int(0), Int(5), Int(5)};
    const UniqueKey on_a = KeyOf(access, access.indexes[0], row);
    const txn::LockKey a = UniqueKeyUnit(access, access.indexes[0], on_a);
    const txn::LockKey b =
        UniqueKeyUnit(access, access.indexes[1], KeyOf(access, access.indexes[1], row));
    EXPECT_EQ(a.unit, txn::LockUnit::kIndexKey);
    EXPECT_EQ(a.rel_oid, access.oid);
    EXPECT_FALSE(a == b) << "one value in two indexes is two units";
    EXPECT_TRUE(a == UniqueKeyUnit(access, access.indexes[0], on_a));
    row[1] = Int(6);
    EXPECT_FALSE(a == UniqueKeyUnit(access, access.indexes[0],
                                    KeyOf(access, access.indexes[0], row)));
}

TEST(IndexKeyUnitTest, ItsIntervalCannotWrapAndOverlapsNoFenceOrTuple) {
    const txn::LockKey top = txn::LockKey::IndexKey(4000, ~std::uint64_t{0});
    EXPECT_LT(top.lo, top.hi);
    const txn::LockKey key = txn::LockKey::IndexKey(4000, 17);
    EXPECT_FALSE(txn::LockKey::Range(4000, 0, std::uint64_t{1} << 48).Overlaps(key));
    EXPECT_FALSE(key.Overlaps(txn::LockKey::Range(4000, 0, std::uint64_t{1} << 48)));
    EXPECT_FALSE(key.Overlaps(txn::LockKey::Tuple(4000, 17)));
    EXPECT_FALSE(key.IsFenceUnit());
    EXPECT_STREQ(txn::LockUnitName(key.unit), "index key");
}

TEST(IndexKeyUnitTest, TwoWritersOfOneKeySerializeAndAFenceDoesNotMeetThem) {
    auto table = txn::LockTable::Create(/*core_count=*/2);
    ASSERT_TRUE(table.ok());
    txn::LockTable& locks = *table.value();
    txn::LockHoldings a;
    txn::LockHoldings b;
    txn::LockHoldings c;
    const txn::LockKey key = txn::LockKey::IndexKey(4000, 17);

    ASSERT_TRUE(locks.TryAcquire(1, key, txn::LockMode::kExclusive, a).value());
    std::uint64_t blocker = 0;
    EXPECT_FALSE(locks.TryAcquire(2, key, txn::LockMode::kExclusive, b, &blocker).value());
    EXPECT_EQ(blocker, 1u);

    // A whole-relation `X` fence is taken while the key unit is held, and
    // the key unit is asked under a held fence: neither meets the other.
    ASSERT_TRUE(locks.TryAcquire(3, txn::LockKey::Range(4000, 0, std::uint64_t{1} << 48),
                                 txn::LockMode::kExclusive, c)
                    .value());
    locks.ReleaseOne(1, key, txn::LockMode::kExclusive, a);
    EXPECT_TRUE(locks.TryAcquire(2, key, txn::LockMode::kExclusive, b).value());
    locks.Release(2, b);
    locks.Release(3, c);
}

}  // namespace
}  // namespace kds::exec
