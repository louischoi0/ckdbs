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
    ASSERT_EQ(Run("CREATE TABLE accounts (id int64, owner varchar) HEAP").substr(0, 7), "CREATED");

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

// ---- Colocation (F5), converted at AH-T4 ---------------------------------
//
// F5 read "parent and child must be owned by the same core" and refused
// otherwise, because the forward check descended the parent locally and had
// nowhere else to ask. It has somewhere now (§2a), so the pair is admitted
// and colocation is advice - the cheaper shape, asked for with a namespace
// (AF-P5) - rather than a gate.
//
// This cell is the conversion itself, kept where the refusal was: it fails
// the day something re-refuses a cross-owner pair without amending F5.
TEST(ForeignKeyColocation, AdmitsRelationsOnDifferentCores) {
    catalog::TableAccess parent{};
    parent.oid = 4001;
    parent.owner_core = 0;
    catalog::TableAccess child{};
    child.oid = 4002;
    child.owner_core = 1;

    EXPECT_TRUE(catalog::CheckForeignKeyColocation(parent, child).ok())
        << "a cross-owner foreign key was refused after AH-T4 converted F5";

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

// ---- AH-T1: the hoist changes no answer (§2a, H-AH1) --------------------
//
// The forward check's descent moved out of the write scope to an extraction
// pass that runs before any row work. These cells are the two places that
// move could have changed an answer, and the one place it must not have
// changed a refusal's shape.

// **The zero-match UPDATE.** Resolving is not failing: an UPDATE whose SET
// names a parent that does not exist, but which matches no row, must still
// answer `UPDATED 0` - because the per-row check it used to run never ran.
// Hoisting the *verdict* rather than only the descent would turn this into
// an FK_VIOLATION that no version of this engine has ever reported.
TEST_F(ForeignKeyCheckTest, AnUpdateMatchingNoRowsDoesNotReportTheHoistedVerdict) {
    ASSERT_EQ(Run("INSERT INTO trades VALUES (1, 100)").substr(0, 8), "INSERTED");

    const std::string out = Run("UPDATE trades SET account_id = 99 WHERE qty = 12345");
    EXPECT_EQ(out, "UPDATED 0") << out;

    // And the same statement against a row that *does* match still refuses,
    // so the cell above is not passing because the check stopped running.
    const std::string matched = Run("UPDATE trades SET account_id = 99 WHERE qty = 100");
    EXPECT_EQ(matched.substr(0, 16), "ERR FK_VIOLATION") << matched;
}

// **The self-referencing foreign key is unreachable through the DDL
// surface**, and the extraction pass carves it out anyway.
//
// Why it matters to AH-T1: a self-referencing parent can be written by the
// *same statement* that checks it - `INSERT INTO nodes VALUES (1, NULL),
// (2, 1)` - so a verdict resolved before any row work would answer "no such
// parent" where the per-row check, running after row 1 landed, answers
// pass. That is the one shape the hoist would change, and
// `ResolveForeignKeyParents` skips it for that reason.
//
// This cell asserts the *unreachability*, because that is the fact today:
// `REFERENCES nodes` inside `CREATE TABLE nodes` cannot resolve, the parent
// not existing yet, and nothing else declares a foreign key. If that ever
// changes, this cell fails and points at a carve-out already waiting rather
// than at a wrong answer nobody was looking for.
//
// The carve-out costs AH nothing either way: parent and child being one
// relation means one `owner_core`, so a self-referencing foreign key can
// never be foreign and its descent never needs to cross.
TEST_F(ForeignKeyCheckTest, ASelfReferencingForeignKeyCannotBeDeclared) {
    const std::string out =
        Run("CREATE TABLE nodes (id int64, parent int64 REFERENCES nodes, label int64) BTREE");
    EXPECT_EQ(out.rfind("ERR", 0), 0u)
        << "a self-referencing foreign key became declarable; "
           "ResolveForeignKeyParents' carve-out is now live code and wants a cell that "
           "exercises it: " << out;
}

// **The refusal still names the row that caused it.** The resolution is
// hoisted over every row; the verdict is applied per row, so a bulk insert
// whose third row is bad still says so about the third row.
TEST_F(ForeignKeyCheckTest, ABulkInsertRefusesOnTheRowThatNamesTheMissingParent) {
    const std::string out =
        Run("INSERT INTO trades VALUES (1, 10), (2, 20), (99, 30), (1, 40)");
    EXPECT_EQ(out.substr(0, 16), "ERR FK_VIOLATION") << out;
    EXPECT_NE(out.find("(row 3)"), std::string::npos) << out;
}

// **AH-R2's deduplication, read off `SHOW ACCESS`.** Twenty children of one
// parent resolve that parent once. The counter moved because the work did -
// before the hoist this reported twenty lookups on `accounts`.
TEST_F(ForeignKeyCheckTest, ManyChildrenOfOneParentResolveItOnce) {
    std::string sql = "INSERT INTO trades VALUES (1, 1)";
    for (int k = 2; k <= 20; ++k) sql += ", (1, " + std::to_string(k) + ")";
    const std::string inserted = Run(sql);
    ASSERT_EQ(inserted.substr(0, 8), "INSERTED") << inserted;

    // One Lookup recorded against the parent, not twenty. `uses=` is the
    // shape's counter (`SHOW ACCESS`), and before the hoist this read
    // `uses=20`.
    const std::string access = Run("SHOW ACCESS");
    EXPECT_NE(access.find("kind=Lookup rel=accounts"), std::string::npos) << access;
    EXPECT_NE(access.find("kind=Lookup rel=accounts columns=[id] uses=1"), std::string::npos)
        << "the parent was resolved once per row rather than once: " << access;
}

// ---- AH-T2, first slice: a foreign parent is grouped, never descended ---
//
// `CheckParentPresent` descends `parent.desc_page_id` with no ownership
// question anywhere in it, so on a parent this core does not own it would
// fault a page it may not fault - or worse, answer from one. The extraction
// pass now asks ownership **before** the descent and defers a foreign
// parent into its owner's group; nothing sends that group yet, so the
// statement is refused instead.
//
// **Tested here and not through the dispatcher, because the dispatcher path
// is unreachable - behind two refusals that fire earlier.** Worth recording
// rather than leaving to be discovered: `CheckForeignKeyColocation` refuses
// a cross-owner declaration (it converts at AH-T4), and a dispatcher on a
// core that owns neither relation is turned away first by the peer-write
// refusal - *"this transaction's writes are bound to core 1 and relation
// 'trades' is owned by core 0"*, which is what a first draft of this cell
// caught while appearing to test the new code. A foreign parent therefore
// arises only where migration separated an already-declared pair - AH-R6's
// "relations split from their parents by history" - which nothing builds
// for user relations yet.
//
// Same device as the colocation cell above, for the same stated reason. The
// grouping it exercises is what AH-T2's sender consumes.
TEST(FkParentVerdicts, ForeignParentsGroupByOwnerAndDeduplicate) {
    exec::FkParentVerdicts held;
    EXPECT_FALSE(held.has_foreign());

    held.Defer(/*owner_core=*/1, /*parent_rel=*/4001, /*parent_pk=*/7);
    held.Defer(/*owner_core=*/1, /*parent_rel=*/4001, /*parent_pk=*/7);  // second row, one parent
    held.Defer(/*owner_core=*/1, /*parent_rel=*/4001, /*parent_pk=*/9);
    held.Defer(/*owner_core=*/2, /*parent_rel=*/4002, /*parent_pk=*/7);  // same pk, other owner

    ASSERT_TRUE(held.has_foreign());
    // **Two groups, not four entries**: the unit is the owner, which is why
    // a statement's cross-owner cost counts owners and not rows (AH-R2).
    ASSERT_EQ(held.foreign().size(), 2u);
    EXPECT_EQ(held.foreign()[0].owner_core, 1u);
    EXPECT_EQ(held.foreign()[0].parents.size(), 2u);
    EXPECT_EQ(held.foreign()[1].owner_core, 2u);
    EXPECT_EQ(held.foreign()[1].parents.size(), 1u);

    // Deferring does not resolve. A caller reading the absence of a verdict
    // as a pass would be the exact hole this grouping exists to close.
    EXPECT_EQ(held.Find(4001, 7), nullptr);
    EXPECT_TRUE(held.empty());
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

// ---- The reverse check under a split child (SA-T6's prerequisite) ----
//
// `RangeEligible`'s `kForeignKey` arm gates a split on either side of an
// FK, so nothing below is reachable through the shipped surface; the
// directory rows are written directly, which is the state SA-T6 makes
// ordinary the day it lifts that gate. Both cells exist because RESTRICT
// needs an authoritative *"no children"* (F6), and a reverse check that
// saw less than the whole child and answered `kPass` would not be a slow
// constraint - it would be an absent one.

// Splits `name` at `lo`, giving the upper range to `owner`.
void SplitChild(catalog::Catalog& catalog, const char* name, std::uint64_t lo,
                std::uint32_t owner) {
    auto oid = catalog.FindTableOidByName(name, nullptr);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto head = catalog.CreateRangeEntryPage(oid.value(), lo);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(catalog.OpenRangeRows(oid.value(), lo, owner, head.value()).ok());
}

TEST_F(ForeignKeyCheckTest, AChildRangeOnAnotherCoreRefusesRatherThanPassing) {
    // The child is heap: D1 declines every btree relation a directory, so
    // a btree child could never reach this arm.
    ASSERT_EQ(Run("CREATE TABLE trades_h (id int64, account_id int64 REFERENCES accounts, "
                  "qty int64)")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades_h VALUES (2, 100)").substr(0, 8), "INSERTED");

    // A range this core does not own. Everything below it - the whole of
    // account 1's child population - is still visible here, and that is
    // exactly the trap: a walk of what is visible answers "no children"
    // for a parent whose children may sit in the range that is not.
    SplitChild(boot_->catalog, "trades_h", /*lo=*/4096, /*owner=*/1);

    const std::string refused = Run("DELETE FROM accounts WHERE id = 1");
    EXPECT_EQ(refused.rfind("ERR ", 0), 0u) << refused;
    EXPECT_NE(refused.find("reverse check"), std::string::npos) << refused;

    // And the parent really is still there: a refusal, not a deletion that
    // reported an error.
    EXPECT_EQ(RowCount("SELECT * FROM accounts WHERE id = 1"), 1u);
}

TEST_F(ForeignKeyCheckTest, AChildInASecondOwnedRangeStillBlocksTheParent) {
    // The cell that bites. Every range is this core's, so the check may
    // answer - but the referencing row lives in the **second** chain, and
    // `desc_page_id` alone is the first. A walk that stopped there would
    // report "no children", delete the parent, and leave a dangling
    // foreign key with nothing logged.
    ASSERT_EQ(Run("CREATE TABLE trades_h (id int64, account_id int64 REFERENCES accounts, "
                  "qty int64)")
                  .substr(0, 7),
              "CREATED");
    // Ids 1 and 2, both below the boundary, and neither references
    // account 1 - so the first chain is a walk that finds nothing.
    ASSERT_EQ(Run("INSERT INTO trades_h VALUES (2, 100)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades_h VALUES (2, 200)").substr(0, 8), "INSERTED");

    SplitChild(boot_->catalog, "trades_h", /*lo=*/3, /*owner=*/0);

    // Id 3: the first row of the second chain, and the only reference to
    // account 1 anywhere.
    ASSERT_EQ(Run("INSERT INTO trades_h VALUES (1, 300)").substr(0, 8), "INSERTED");

    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 1").substr(0, 16), "ERR FK_VIOLATION");
    EXPECT_EQ(RowCount("SELECT * FROM accounts WHERE id = 1"), 1u);

    // The converse, so the cell is not passing by refusing everything:
    // account 2 is referenced only from the first chain, and deleting it
    // is still refused for the ordinary reason.
    EXPECT_EQ(Run("DELETE FROM accounts WHERE id = 2").substr(0, 16), "ERR FK_VIOLATION");
}

// The **Cabin** arm of that same refusal, and the reason it is asked
// before the serve rather than beside the walk. An exhausted entry set is
// an authoritative "no children" (F6) - and since SB-R1 it is
// authoritative for *(observed value x the ranges its core owns)* alone
// (`docs/spec/cabin.md` §4b). A set observed while the relation was whole
// answers `kPass` for ranges it no longer speaks for, and no walk runs to
// contradict it: a guard placed below the serve would be a guard this
// shape never reaches. Account 2's set is observed empty *before* the
// boundary opens, which is that state exactly.
TEST_F(ForeignKeyCheckTest, ACabinCannotAnswerForAChildRangeOnAnotherCore) {
    ASSERT_EQ(Run("CREATE TABLE trades_h (id int64, account_id int64 REFERENCES accounts, "
                  "qty int64)")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE CABIN ON trades_h(account_id)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades_h VALUES (1, 100)").substr(0, 8), "INSERTED");
    // A declared Cabin observes on the first probe, and the set it banks
    // for account 2 is empty - the answer the serve exists to give.
    ASSERT_EQ(RowCount("SELECT * FROM trades_h WHERE account_id = 2"), 0u);

    SplitChild(boot_->catalog, "trades_h", /*lo=*/4096, /*owner=*/1);

    const std::string refused = Run("DELETE FROM accounts WHERE id = 2");
    EXPECT_EQ(refused.rfind("ERR ", 0), 0u) << refused;
    EXPECT_NE(refused.find("reverse check"), std::string::npos) << refused;
    EXPECT_EQ(RowCount("SELECT * FROM accounts WHERE id = 2"), 1u);
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

// ---- AF-T4: the namespace remedy, spoken where a user is choosing --------
//
// `instructions/v2.8.0/ratification-af-namespace.md` AF-P5/AF-T4. AH-T4
// converted `CheckForeignKeyColocation` from a constraint to a
// recommendation - a cross-owner pair is admitted and priced - and the
// recommendation had nowhere to be spoken until `CREATE NAMESPACE` existed.
// It is spoken at CREATE TABLE, which is the moment a user is choosing.

class ForeignKeyPlacementTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Four cores, so two namespaces can be two cores. Namespace
        // placement is the shipped default (`Expeditor::Config`), but a
        // bare bootstrap builds a bare Catalog, so it is set here.
        auto boot = bootstrap::BootstrapDatabase(store_, 1000,
                                                 storage::kDefaultInlineCellWidth, /*cores=*/4);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        boot_->catalog.SetPlacementPolicy(catalog::PlacementPolicy::kNamespace);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(std::string_view line) { return dispatcher_->Dispatch(line).response; }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(ForeignKeyPlacementTest, ACrossOwnerForeignKeyIsAdmittedAndSaysWhatItCosts) {
    ASSERT_EQ(Run("CREATE NAMESPACE ledger").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE NAMESPACE trading").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE ledger.accounts (id int64, name varchar) BTREE").substr(0, 7),
              "CREATED");

    const std::string out =
        Run("CREATE TABLE trading.trades (id int64, account_id int64 REFERENCES accounts) BTREE");
    // **Admitted** - the pair is legal since AH-T4, and a refusal here
    // would be the constraint coming back.
    ASSERT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_NE(out.find("WARN"), std::string::npos) << out;
    // Both costs, because "admitted" must not read as "free"...
    EXPECT_NE(out.find("cross-core probe"), std::string::npos) << out;
    EXPECT_NE(out.find("DELETE of a referenced parent row is refused"), std::string::npos) << out;
    // ...and the one thing the user can act on.
    EXPECT_NE(out.find("one namespace"), std::string::npos) << out;
}

TEST_F(ForeignKeyPlacementTest, AForeignKeyInsideOneNamespaceIsSilent) {
    // The point of the advice is that following it costs nothing to say.
    ASSERT_EQ(Run("CREATE NAMESPACE ledger").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE ledger.accounts (id int64, name varchar) BTREE").substr(0, 7),
              "CREATED");

    const std::string out =
        Run("CREATE TABLE ledger.trades (id int64, account_id int64 REFERENCES accounts) BTREE");
    ASSERT_EQ(out.substr(0, 7), "CREATED") << out;
    EXPECT_EQ(out.find("WARN"), std::string::npos) << out;

    // And the reason it is silent: NS10 put them on one core.
    auto parent = boot_->catalog.FindTableOidByName("accounts");
    auto child = boot_->catalog.FindTableOidByName("trades");
    ASSERT_TRUE(parent.ok());
    ASSERT_TRUE(child.ok());
    auto parent_row = boot_->catalog.GetSysTableRow(parent.value());
    auto child_row = boot_->catalog.GetSysTableRow(child.value());
    ASSERT_TRUE(parent_row.ok());
    ASSERT_TRUE(child_row.ok());
    EXPECT_EQ(parent_row.value().owner_core, child_row.value().owner_core);
    EXPECT_NE(parent_row.value().owner_core, catalog::kSystemCore)
        << "a declared namespace stayed on the system core, so this proves nothing";
}

}  // namespace
}  // namespace kds::server
