#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/foreign_key.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// **FK-M1: the catalog and the DDL surface** (docs/spec/foreign-keys.md §1).
//
// What this milestone owes, and therefore what this file proves in three
// groups: a foreign key is **declarable**, **introspectable**, and
// **rejectable** - with the refusals tested one by one, because a
// constraint surface is judged by what it will not accept.
//
// Nothing here checks a constraint. FK-M1 records that a foreign key
// exists; the forward check (FK-M2) and the reverse check (FK-M3) are what
// make it mean something at write time, and a test asserting otherwise
// would be asserting a promise this milestone deliberately does not make.

namespace kds::server {
namespace {

class ForeignKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(std::string_view line) { return dispatcher_->Dispatch(line).response; }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The row (docs/spec/foreign-keys.md §1) -------------------------------

TEST(SysFkeyRow, RoundTripsThroughItsCodec) {
    catalog::SysFkeyRow row{};
    row.fk_id = 77;
    row.child_rel_oid = 4001;
    row.parent_rel_oid = 4002;
    row.child_column_no = 3;
    row.flags = catalog::kFkNullable;

    const auto bytes = row.Encode();
    auto decoded = catalog::SysFkeyRow::Decode(bytes);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().fk_id, row.fk_id);
    EXPECT_EQ(decoded.value().child_rel_oid, row.child_rel_oid);
    EXPECT_EQ(decoded.value().parent_rel_oid, row.parent_rel_oid);
    EXPECT_EQ(decoded.value().child_column_no, row.child_column_no);
    EXPECT_EQ(decoded.value().flags, row.flags);
}

TEST(SysFkeyRow, RefusesAPayloadOfTheWrongSize) {
    std::array<std::byte, catalog::SysFkeyRow::kOnDiskSize - 1> short_row{};
    auto decoded = catalog::SysFkeyRow::Decode(short_row);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

// A zeroed row - which is what empty page bytes decode to - must not read
// as a nullable foreign key. The check runs by default; only an explicit
// flag turns the NULL skip on.
TEST(SysFkeyRow, AZeroedRowIsNotNullable) {
    catalog::SysFkeyRow row{};
    EXPECT_EQ(row.flags & catalog::kFkNullable, 0u);
}

// ---- Declarable ----------------------------------------------------------

TEST_F(ForeignKeyTest, ReferencesDeclaresAForeignKey) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts, "
                  "qty int64) BTREE")
                  .substr(0, 7),
              "CREATED");

    auto rows = boot_->catalog.ListForeignKeys();
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows.value().size(), 1u);

    auto accounts = boot_->catalog.FindTableOidByName("accounts");
    auto trades = boot_->catalog.FindTableOidByName("trades");
    ASSERT_TRUE(accounts.ok());
    ASSERT_TRUE(trades.ok());

    const catalog::SysFkeyRow& fk = rows.value().front();
    EXPECT_EQ(fk.child_rel_oid, trades.value());
    EXPECT_EQ(fk.parent_rel_oid, accounts.value());
    EXPECT_EQ(fk.child_column_no, 1u);
    // account_id is NOT NULL by default (null.md D1), so the
    // declaration stamps no kFkNullable.
    EXPECT_EQ(fk.flags, 0u);
    EXPECT_NE(fk.fk_id, 0u);
}

// Both ends, from one scan: the child knows what it references and the
// parent knows who references it. The second is the one that makes the
// catalog-version bump necessary - creating `trades` stales a cached entry
// for `accounts`, a relation the CREATE statement never names.
TEST_F(ForeignKeyTest, BothRelationsCarryTheForeignKey) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");

    // Opened - and therefore cached - *before* the child exists, which is
    // exactly the entry a missing invalidation would leave stale.
    auto parent_oid = boot_->catalog.FindTableOidByName("accounts");
    ASSERT_TRUE(parent_oid.ok());
    ASSERT_TRUE(boot_->catalog.InitTableAccess(parent_oid.value()).ok());

    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .substr(0, 7),
              "CREATED");

    auto child_oid = boot_->catalog.FindTableOidByName("trades");
    ASSERT_TRUE(child_oid.ok());

    auto child = boot_->catalog.InitTableAccess(child_oid.value());
    ASSERT_TRUE(child.ok());
    ASSERT_EQ(child.value()->fkeys_out.size(), 1u);
    EXPECT_EQ(child.value()->fkeys_out.front().rel_oid, parent_oid.value());
    EXPECT_EQ(child.value()->fkeys_out.front().column_no, 1u);
    EXPECT_TRUE(child.value()->fkeys_in.empty());

    auto parent = boot_->catalog.InitTableAccess(parent_oid.value());
    ASSERT_TRUE(parent.ok());
    ASSERT_EQ(parent.value()->fkeys_in.size(), 1u);
    EXPECT_EQ(parent.value()->fkeys_in.front().rel_oid, child_oid.value());
    EXPECT_EQ(parent.value()->fkeys_in.front().column_no, 1u);
    EXPECT_TRUE(parent.value()->fkeys_out.empty());

    const catalog::ForeignKeyRef* on_column = child.value()->ForeignKeyOn(1);
    ASSERT_NE(on_column, nullptr);
    EXPECT_EQ(on_column->rel_oid, parent_oid.value());
    EXPECT_EQ(child.value()->ForeignKeyOn(0), nullptr);
    EXPECT_EQ(child.value()->ForeignKeyOn(7), nullptr);
}

TEST_F(ForeignKeyTest, ARelationMayDeclareSeveralForeignKeys) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE symbols (id int64, name varchar) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts, "
                  "symbol_id int64 REFERENCES symbols) BTREE")
                  .substr(0, 7),
              "CREATED");

    auto rows = boot_->catalog.ListForeignKeys();
    ASSERT_TRUE(rows.ok());
    EXPECT_EQ(rows.value().size(), 2u);
    // Distinct ids from the sys.fkeys sequence, not two rows sharing one.
    EXPECT_NE(rows.value()[0].fk_id, rows.value()[1].fk_id);
}

// A foreign key alongside a cabin on the same column: two optional suffixes
// in the fixed order the grammar states.
TEST_F(ForeignKeyTest, ReferencesComposesWithACabinClause) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts CABIN) "
                  "BTREE")
                  .substr(0, 7),
              "CREATED");

    auto fkeys = boot_->catalog.ListForeignKeys();
    ASSERT_TRUE(fkeys.ok());
    EXPECT_EQ(fkeys.value().size(), 1u);
    auto cabins = boot_->catalog.ListCabins();
    ASSERT_TRUE(cabins.ok());
    EXPECT_EQ(cabins.value().size(), 1u);
}

// The word is unreserved, exactly like CABIN beside it: a column may still
// be called `references`.
TEST_F(ForeignKeyTest, ReferencesIsNotAReservedWord) {
    EXPECT_EQ(Run("CREATE TABLE t (id int64, references int64) BTREE").substr(0, 7), "CREATED");
    auto fkeys = boot_->catalog.ListForeignKeys();
    ASSERT_TRUE(fkeys.ok());
    EXPECT_TRUE(fkeys.value().empty());
}

// ---- Introspectable ------------------------------------------------------

TEST_F(ForeignKeyTest, ShowFkeysListsWhatWasDeclared) {
    EXPECT_EQ(Run("SHOW FKEYS"), "fkeys=0");

    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .substr(0, 7),
              "CREATED");

    const std::string out = Run("SHOW FKEYS");
    EXPECT_NE(out.find("fkeys=1"), std::string::npos) << out;
    EXPECT_NE(out.find("child=trades"), std::string::npos) << out;
    EXPECT_NE(out.find("column=account_id"), std::string::npos) << out;
    EXPECT_NE(out.find("parent=accounts"), std::string::npos) << out;
    EXPECT_NE(out.find("action=RESTRICT"), std::string::npos) << out;
    EXPECT_NE(out.find("nullable=no"), std::string::npos) << out;
}

TEST_F(ForeignKeyTest, DescribeNamesTheReferencedRelation) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts, "
                  "qty int64) BTREE")
                  .substr(0, 7),
              "CREATED");

    const std::string out = Run("DESCRIBE trades");
    EXPECT_NE(out.find("name=account_id"), std::string::npos) << out;
    EXPECT_NE(out.find("references=accounts"), std::string::npos) << out;
    // Only the referencing column carries the annotation.
    EXPECT_EQ(out.find("references=accounts", out.find("references=accounts") + 1),
              std::string::npos)
        << out;
}

// ---- Rejectable ----------------------------------------------------------

// The pre-check's whole purpose: a refused declaration leaves nothing
// behind, because there is no DROP TABLE to clean up with.
TEST_F(ForeignKeyTest, AnUnknownParentRefusesAndCreatesNothing) {
    const std::string out = Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES "
                                "nosuch) BTREE");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("nosuch"), std::string::npos) << out;
    EXPECT_FALSE(boot_->catalog.FindTableOidByName("trades").ok());
}

// F1 puts the reference on the parent's Keystone id, and a heap relation
// has no index for it - so every check would scan the parent.
TEST_F(ForeignKeyTest, AHeapParentIsRefused) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar)").substr(0, 7), "CREATED");

    const std::string out =
        Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts) BTREE");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("BTREE"), std::string::npos) << out;
    EXPECT_FALSE(boot_->catalog.FindTableOidByName("trades").ok());
}

TEST_F(ForeignKeyTest, AColumnThatCannotHoldAKeystoneIdIsRefused) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");

    const std::string out =
        Run("CREATE TABLE trades (id int64, account varchar REFERENCES accounts) BTREE");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("Keystone id"), std::string::npos) << out;
    EXPECT_FALSE(boot_->catalog.FindTableOidByName("trades").ok());
}

// The pk is the row's identity, not a field of it: a reference stored there
// would make one row's identity a statement about another row.
TEST_F(ForeignKeyTest, AForeignKeyOnThePrimaryKeyIsRefused) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");

    const std::string out =
        Run("CREATE TABLE trades (id int64 REFERENCES accounts, qty int64) BTREE");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(out.find("primary key"), std::string::npos) << out;
    EXPECT_FALSE(boot_->catalog.FindTableOidByName("trades").ok());
}

// `REFERENCES parent(col)` names the only column it could name, or one the
// engine cannot reference. Both are refused at the '(' rather than parsed
// and then argued with.
TEST_F(ForeignKeyTest, AParentColumnListIsRefusedWithAPosition) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");

    auto parsed = parser::Parse(
        "CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts(id)) BTREE");
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(parsed.status().message().find("byte "), std::string::npos)
        << parsed.status().message();
}

// The catalog is the one door, so it refuses a second foreign key on a
// column whoever asks - the DDL surface cannot express this today, which is
// exactly why the check does not live there.
TEST_F(ForeignKeyTest, ASecondForeignKeyOnOneColumnIsRefused) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE symbols (id int64, name varchar) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .substr(0, 7),
              "CREATED");

    auto trades = boot_->catalog.FindTableOidByName("trades");
    auto symbols = boot_->catalog.FindTableOidByName("symbols");
    ASSERT_TRUE(trades.ok());
    ASSERT_TRUE(symbols.ok());

    auto second = boot_->catalog.CreateForeignKey(trades.value(), 1, symbols.value());
    EXPECT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kAlreadyExists);
}

TEST_F(ForeignKeyTest, TheCatalogRefusesADeclarationTheDdlSurfaceWouldHaveCaught) {
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64) BTREE").substr(0, 7),
              "CREATED");

    auto trades = boot_->catalog.FindTableOidByName("trades");
    auto accounts = boot_->catalog.FindTableOidByName("accounts");
    ASSERT_TRUE(trades.ok());
    ASSERT_TRUE(accounts.ok());

    // Column 0 - the pk - and a column past the schema, refused at the door
    // rather than only at the CREATE TABLE clause.
    EXPECT_EQ(boot_->catalog.CreateForeignKey(trades.value(), 0, accounts.value()).status().code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(boot_->catalog.CreateForeignKey(trades.value(), 9, accounts.value()).status().code(),
              StatusCode::kInvalidArgument);
    // And an unknown relation on either side.
    EXPECT_EQ(boot_->catalog.CreateForeignKey(trades.value(), 1, 999999).status().code(),
              StatusCode::kNotFound);
}

// ---- Colocation (F5) -----------------------------------------------------
//
// Tested against the check itself rather than through DDL, because
// AssignOwnerCore() puts every relation on the creating core today: there is
// no way to *create* a cross-core pair, and the check exists for when there
// is.
TEST(ForeignKeyColocation, RefusesRelationsOnDifferentCores) {
    catalog::TableAccess parent{};
    parent.oid = 4001;
    parent.owner_core = 0;
    catalog::TableAccess child{};
    child.oid = 4002;
    child.owner_core = 1;

    Status refused = catalog::CheckForeignKeyColocation(parent, child);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kUnsupported);
    // Both cores named, so the reply says what would have to move.
    EXPECT_NE(refused.message().find("core 1"), std::string::npos) << refused.message();
    EXPECT_NE(refused.message().find("core 0"), std::string::npos) << refused.message();

    child.owner_core = 0;
    EXPECT_TRUE(catalog::CheckForeignKeyColocation(parent, child).ok());
}


// ---- FK-M2 / FK-M3 / FK-M5: the checks -----------------------------------
//
// A second fixture, with a transaction manager and a Cabin store, because
// the checks are where transactions and Cabins both start to matter: the
// verdict for an in-flight writer is F3's whole point, and the reverse
// check's fast path is F6's.

class ForeignKeyCheckTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        cabins_.emplace(stats::CabinLimits{});

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, &*cabins_, &*mgr_);

        ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) BTREE").substr(0, 7),
                  "CREATED");
        ASSERT_EQ(Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts, "
                      "qty int64) BTREE")
                      .substr(0, 7),
                  "CREATED");
        ASSERT_EQ(Run("INSERT INTO accounts VALUES ('ada')").substr(0, 8), "INSERTED");
        ASSERT_EQ(Run("INSERT INTO accounts VALUES ('grace')").substr(0, 8), "INSERTED");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    // How many rows a SELECT answered. The reply is one wire line - a
    // header, then one `\n`-escaped section per row - so the count is the
    // number of sections after the first.
    std::size_t RowCount(const std::string& sql) {
        const std::string reply = Run(sql);
        std::size_t rows = 0;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;
             at = reply.find("\\n", at + 2)) {
            ++rows;
        }
        return rows;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<stats::CabinStore> cabins_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The forward check (§2, FK-M2) ---------------------------------------

TEST_F(ForeignKeyCheckTest, AChildReferencingALiveParentIsAccepted) {
    EXPECT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
}

TEST_F(ForeignKeyCheckTest, AChildReferencingNoParentIsRefused) {
    const std::string out = Run("INSERT INTO trades VALUES (99, 100)");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;
    // Non-retryable, and it says so where a client reads it: re-running this
    // statement fails the same way.
    EXPECT_NE(out.find("retryable=0"), std::string::npos) << out;
    EXPECT_NE(out.find("account_id"), std::string::npos) << out;
    EXPECT_NE(out.find("accounts"), std::string::npos) << out;

    // And nothing was written.
    EXPECT_EQ(RowCount("SELECT * FROM trades"), 0u);
}

// Latest state, not the statement's snapshot: a parent deleted and committed
// is gone for a check even though a snapshot taken earlier could still see
// it. This is the case §4 exists for.
TEST_F(ForeignKeyCheckTest, AChildReferencingADeletedParentIsRefused) {
    ASSERT_EQ(Run("DELETE FROM accounts WHERE id = 2"), "DELETED 1");
    const std::string out = Run("INSERT INTO trades VALUES (2, 100)");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;
}

// The fourth row of §4's table: a transaction's own uncommitted parent
// satisfies its own child, with no special case in the predicate.
TEST_F(ForeignKeyCheckTest, ATransactionSeesItsOwnParent) {
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO accounts VALUES ('hopper')").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run(s, "INSERT INTO trades VALUES (3, 7)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
}

// F3: an in-flight writer is *seen* and refused immediately, retryably. No
// waiting - there is nothing to wait on under a cooperative core.
TEST_F(ForeignKeyCheckTest, AParentWrittenByAnotherLiveTransactionIsBusy) {
    Session writer;
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "INSERT INTO accounts VALUES ('turing')").substr(0, 8), "INSERTED");

    Session other;
    const std::string out = Run(other, "INSERT INTO trades VALUES (3, 7)");
    EXPECT_EQ(out.substr(0, 17), "ERR TXN_CONFLICT ") << out;
    EXPECT_NE(out.find("retryable=1"), std::string::npos) << out;

    // And it is a real retry, not a permanent refusal: once the writer
    // commits, the same statement succeeds.
    ASSERT_EQ(Run(writer, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Run(other, "INSERT INTO trades VALUES (3, 7)").substr(0, 8), "INSERTED");
}

// A rolled-back parent never existed, so a child naming it is a violation
// rather than a conflict - the same id, a different answer, decided by how
// the other transaction ended.
TEST_F(ForeignKeyCheckTest, AParentFromARolledBackTransactionIsAViolation) {
    Session writer;
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "INSERT INTO accounts VALUES ('lovelace')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(writer, "ROLLBACK").substr(0, 8), "ROLLBACK");

    const std::string out = Run("INSERT INTO trades VALUES (3, 7)");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;
}

// ---- UPDATE of an fk column (§2, FK-M3) ----------------------------------

TEST_F(ForeignKeyCheckTest, UpdatingAnFkColumnIsChecked) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");

    EXPECT_EQ(Run("UPDATE trades SET account_id = 2 WHERE id = 1"), "UPDATED 1");

    const std::string out = Run("UPDATE trades SET account_id = 99 WHERE id = 1");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;

    // The refused UPDATE changed nothing: the row still names account 2.
    EXPECT_NE(Run("SELECT * FROM trades WHERE id = 1").find("1,2,"), std::string::npos);
}

TEST_F(ForeignKeyCheckTest, AnUpdateThatTouchesNoFkColumnIsNotChecked) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("UPDATE trades SET qty = 250 WHERE id = 1"), "UPDATED 1");
}

// An UPDATE whose WHERE matches nothing runs no check, because there is no
// row for the constraint to be about.
TEST_F(ForeignKeyCheckTest, AnUpdateMatchingNoRowIsNotChecked) {
    EXPECT_EQ(Run("UPDATE trades SET account_id = 99 WHERE id = 42"), "UPDATED 0");
}

// ---- The reverse check (§3, FK-M3) ---------------------------------------

TEST_F(ForeignKeyCheckTest, DeletingAReferencedParentIsRefused) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");

    const std::string out = Run("DELETE FROM accounts WHERE id = 1");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;
    EXPECT_NE(out.find("trades.account_id"), std::string::npos) << out;

    // RESTRICT, so the parent is still there.
    EXPECT_EQ(RowCount("SELECT * FROM accounts WHERE id = 1"), 1u);
}

TEST_F(ForeignKeyCheckTest, DeletingAnUnreferencedParentIsAllowed) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 2"), "DELETED 1");
}

// A delete-marked child is not a reference: the parent becomes deletable
// again, which is the ordinary way out of a RESTRICT.
TEST_F(ForeignKeyCheckTest, DeletingTheChildFirstFreesTheParent) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("DELETE FROM accounts WHERE id = 1").substr(0, 16), "ERR FK_VIOLATION");
    ASSERT_EQ(Run("DELETE FROM trades WHERE id = 1"), "DELETED 1");
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 1"), "DELETED 1");
}

// The mirror of the forward busy case, on the other side of the constraint.
TEST_F(ForeignKeyCheckTest, AChildWrittenByAnotherLiveTransactionIsBusy) {
    Session writer;
    ASSERT_EQ(Run(writer, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(writer, "INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");

    Session other;
    const std::string out = Run(other, "DELETE FROM accounts WHERE id = 1");
    EXPECT_EQ(out.substr(0, 17), "ERR TXN_CONFLICT ") << out;
    EXPECT_NE(out.find("retryable=1"), std::string::npos) << out;
}

// A relation nothing references is not slowed down by the machinery, and -
// more to the point - is not touched by it at all.
TEST_F(ForeignKeyCheckTest, ARelationWithNoForeignKeysIsUnaffected) {
    ASSERT_EQ(Run("CREATE TABLE notes (id int64, body varchar) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO notes VALUES ('hello')").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("UPDATE notes SET body = 'bye' WHERE id = 1"), "UPDATED 1");
    EXPECT_EQ(Run("DELETE FROM notes WHERE id = 1"), "DELETED 1");
}

// ---- FK-M4: the checks show up in the statistics --------------------------

TEST_F(ForeignKeyCheckTest, ChecksAreVisibleInShowAccess) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("DELETE FROM accounts WHERE id = 2"), "DELETED 1");

    const std::string out = Run("SHOW ACCESS");
    // The forward check is a pk lookup on the parent; the reverse check is a
    // filtered walk of the child. Both relations appear, which is the point:
    // an operator can see what the constraints cost.
    EXPECT_NE(out.find("rel=accounts"), std::string::npos) << out;
    EXPECT_NE(out.find("rel=trades"), std::string::npos) << out;
}

// ---- FK-M5: the reverse check reads a Cabin -------------------------------

TEST_F(ForeignKeyCheckTest, AnObservedCabinAnswersTheReverseCheckWithoutWalking) {
    ASSERT_EQ(Run("CREATE CABIN ON trades(account_id)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");

    // The Cabin's values arrive the ordinary way - a query that filters
    // children by parent id. A declared Cabin observes on the first probe.
    ASSERT_EQ(RowCount("SELECT * FROM trades WHERE account_id = 2"), 0u);

    const std::uint64_t hits_before = cabins_->stats().hits;
    // Account 2 has no children, and the observed empty set is an
    // authoritative "no children" - the one answer no advisory structure can
    // give, and the whole of F6.
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 2"), "DELETED 1");
    EXPECT_GT(cabins_->stats().hits, hits_before);

    // The observed set must not be able to *hide* a child: account 1 has one.
    ASSERT_EQ(RowCount("SELECT * FROM trades WHERE account_id = 1"), 1u);
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 1").substr(0, 16), "ERR FK_VIOLATION");
}

// The surplus an append-only set carries has to be subtracted by the reverse
// check too: a child moved off the value is not a reference to it, even
// though its pk is still in that value's entry set (§1's superset rule).
TEST_F(ForeignKeyCheckTest, ACabinSurplusEntryDoesNotBlockADelete) {
    ASSERT_EQ(Run("CREATE CABIN ON trades(account_id)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");
    ASSERT_EQ(RowCount("SELECT * FROM trades WHERE account_id = 1"), 1u);

    // The row moves to account 2. Append-only maintenance leaves its pk in
    // account 1's set, where it no longer belongs.
    ASSERT_EQ(Run("UPDATE trades SET account_id = 2 WHERE id = 1"), "UPDATED 1");

    // Account 1 is now unreferenced, and the stale entry must not say
    // otherwise.
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 1"), "DELETED 1");
}

// ---- NULL fk values: MATCH SIMPLE, both directions (null.md §4) ------

// A NULL child key satisfies the constraint vacuously: the insert probes no
// parent, and the stored NULL never blocks the parent side of anything.
TEST_F(ForeignKeyCheckTest, ANullForeignKeySatisfiesVacuouslyInBothDirections) {
    ASSERT_EQ(Run("CREATE TABLE orders (id int64, account_id int64 NULL "
                  "REFERENCES accounts) BTREE")
                  .substr(0, 7),
              "CREATED");

    // Forward: a NULL fk inserts with no parent existence to prove -
    // account 999 does not exist and is not asked about.
    ASSERT_EQ(Run("INSERT INTO orders VALUES (NULL)").substr(0, 8), "INSERTED");
    // A real value on the same column still enforces.
    const std::string missing = Run("INSERT INTO orders VALUES (999)");
    EXPECT_EQ(missing.substr(0, 16), "ERR FK_VIOLATION") << missing;
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1)").substr(0, 8), "INSERTED");

    // Reverse: account 2 is referenced only by NULLs (that is, by nothing),
    // so its delete passes; account 1 has a real child and refuses.
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 2"), "DELETED 1");
    const std::string blocked = Run("DELETE FROM accounts WHERE id = 1");
    EXPECT_EQ(blocked.substr(0, 16), "ERR FK_VIOLATION") << blocked;

    // An UPDATE to NULL releases the reference; the delete then passes.
    ASSERT_EQ(Run("UPDATE orders SET account_id = NULL WHERE account_id = 1"), "UPDATED 1");
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 1"), "DELETED 1");
}

// The declaration stamps kFkNullable from the column, and SHOW prints it.
TEST_F(ForeignKeyCheckTest, ANullableForeignKeyColumnShowsNullableYes) {
    ASSERT_EQ(Run("CREATE TABLE orders (id int64, account_id int64 NULL "
                  "REFERENCES accounts) BTREE")
                  .substr(0, 7),
              "CREATED");
    const std::string out = Run("SHOW FKEYS");
    EXPECT_NE(out.find("nullable=yes"), std::string::npos) << out;
    // The fixture's NOT NULL fk on trades keeps printing nullable=no beside it.
    EXPECT_NE(out.find("nullable=no"), std::string::npos) << out;
}

}  // namespace
}  // namespace kds::server
