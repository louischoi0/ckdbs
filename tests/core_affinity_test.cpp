#include "kds/server/core_affinity.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// Which core may run a statement (docs/spec/crosscore.md CC3 and §6).
//
// Two rules with different lifetimes, and the tests are grouped that way
// because the difference matters:
//
//   - The **write** restriction is decided and permanent until 2PC: a
//     transaction's writes bind to one core. It survives the pipeline.
//   - The **read** refusal is temporary: it is what the step pipeline will
//     replace, and it exists so a cross-core read names its reason instead
//     of surfacing as a page-store fault.

namespace kds::server {
namespace {

// ---- The session's home-core binding -----------------------------------

TEST(SessionHomeCoreTest, AFreshSessionIsUnbound) {
    Session session;
    EXPECT_FALSE(session.home_bound());
    // Unbound admits a write anywhere - a read-only transaction never
    // acquires a home and never restricts anything.
    EXPECT_TRUE(session.MayWriteOn(0));
    EXPECT_TRUE(session.MayWriteOn(3));
}

TEST(SessionHomeCoreTest, TheFirstWriteBindsAndLaterOnesAreChecked) {
    Session session;
    session.BindHomeCore(2);
    EXPECT_TRUE(session.home_bound());
    EXPECT_EQ(session.home_core(), 2u);

    EXPECT_TRUE(session.MayWriteOn(2));
    EXPECT_FALSE(session.MayWriteOn(0)) << "a second core's write was admitted";
    EXPECT_FALSE(session.MayWriteOn(3));

    // Binding is idempotent for the core already held - a second write to
    // the same core must not look like a rebind.
    session.BindHomeCore(2);
    EXPECT_EQ(session.home_core(), 2u);
}

TEST(SessionHomeCoreTest, TheBindingBelongsToTheTransactionNotTheConnection) {
    // Carrying it across a commit would restrict the next transaction for
    // no reason.
    Session session;
    session.BindHomeCore(1);
    ASSERT_TRUE(session.home_bound());

    session.Finish();
    EXPECT_FALSE(session.home_bound());
    EXPECT_TRUE(session.MayWriteOn(0));
}

// ---- The refusals themselves -------------------------------------------

TEST(CoreAffinityTest, AWriteRefusalIsRetryableAndNamesBothCores) {
    // kTxnConflict, not a new code: from the client's side this is the same
    // situation first-updater-wins produces - it cannot proceed, and a
    // retry may work - so a client that already handles TXN_CONFLICT needs
    // no new code.
    Status s = CrossCoreWriteRefused(/*home=*/1, /*target=*/2, "accounts");
    EXPECT_EQ(s.code(), StatusCode::kTxnConflict);
    EXPECT_NE(s.message().find("core 1"), std::string::npos) << s.message();
    EXPECT_NE(s.message().find("core 2"), std::string::npos) << s.message();
    EXPECT_NE(s.message().find("accounts"), std::string::npos) << s.message();
}

TEST(CoreAffinityTest, AReadRefusalIsNotRetryable) {
    // Retrying changes nothing, and telling a client to retry a statement
    // that can never run here costs it a loop.
    Status s = CrossCoreReadUnsupported(/*this_core=*/0, /*target=*/1, "trades");
    EXPECT_EQ(s.code(), StatusCode::kUnsupported);
    EXPECT_NE(s.message().find("trades"), std::string::npos) << s.message();
    EXPECT_NE(s.message().find("pipeline"), std::string::npos)
        << "the message should say what is missing: " << s.message();
}

// ---- §6's counters -----------------------------------------------------

TEST(CrossCoreWriteCountersTest, RefusalsAreCountedByHomeTargetAndRelation) {
    // §6's "input the future placement/2PC decision will be made from". The
    // question it answers is which relations a workload actually wants to
    // write cross-core, which is not answerable from a single total.
    CrossCoreWriteCounters counters;
    counters.Record(0, 1, 4000);
    counters.Record(0, 1, 4000);
    counters.Record(0, 2, 4000);
    counters.Record(1, 0, 4001);

    EXPECT_EQ(counters.CountFor(0, 1, 4000), 2u);
    EXPECT_EQ(counters.CountFor(0, 2, 4000), 1u);
    EXPECT_EQ(counters.CountFor(1, 0, 4001), 1u);
    EXPECT_EQ(counters.CountFor(0, 1, 4001), 0u) << "an unrecorded key must read as zero";
    EXPECT_EQ(counters.total(), 4u);
}

TEST(CrossCoreWriteCountersTest, IterationOrderIsStable) {
    // Anything observable has to be deterministic run to run (sched.md §8),
    // and these are meant to be reported.
    CrossCoreWriteCounters a;
    CrossCoreWriteCounters b;
    for (std::uint32_t t : {3u, 1u, 2u}) a.Record(0, t, 4000);
    for (std::uint32_t t : {2u, 3u, 1u}) b.Record(0, t, 4000);

    std::vector<std::uint32_t> from_a;
    std::vector<std::uint32_t> from_b;
    for (const auto& [key, count] : a.counts()) from_a.push_back(key.target_core);
    for (const auto& [key, count] : b.counts()) from_b.push_back(key.target_core);
    EXPECT_EQ(from_a, from_b);
    EXPECT_EQ(from_a, (std::vector<std::uint32_t>{1, 2, 3}));
}

// ---- End to end through the dispatcher ---------------------------------

class AffinityDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        db_.emplace(std::move(boot.value()));
    }

    // A dispatcher claiming to run on `core_id`. Every relation created
    // below lands on core 0 (single-core placement), so a dispatcher on any
    // other core is one that owns nothing - which is exactly the situation
    // the guards exist for.
    CommandDispatcher DispatcherOn(std::uint32_t core_id) {
        return CommandDispatcher(db_->superblock, db_->catalog, store_, nullptr, nullptr, nullptr,
                                 wal::DurabilityClass::kGroup, exec::Budget(), nullptr, false,
                                 /*access_statistics=*/false, nullptr, nullptr,
                                 txn::IsolationLevel::kReadCommitted, core_id);
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> db_;
};

TEST_F(AffinityDispatchTest, TheLocalCoreIsUnaffected) {
    // Guideline 2: a single-core build must behave exactly as before, and
    // the guard costs one integer comparison.
    CommandDispatcher core0 = DispatcherOn(0);
    Session session;

    ASSERT_EQ(core0.Dispatch("CREATE TABLE t (id INT64, v INT64)", &session).response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(core0.Dispatch("INSERT INTO t VALUES (7)", &session).response.rfind("ERR", 0),
              std::string::npos);
    EXPECT_EQ(core0.Dispatch("SELECT * FROM t", &session).response.rfind("ERR", 0),
              std::string::npos);
}

TEST_F(AffinityDispatchTest, AWriteToAnotherCoresRelationIsRefusedRetryably) {
    CommandDispatcher core0 = DispatcherOn(0);
    Session setup;
    ASSERT_EQ(core0.Dispatch("CREATE TABLE t (id INT64, v INT64)", &setup).response.rfind("ERR", 0),
              std::string::npos);

    // Core 1 owns nothing, so this write is refused - and refused with the
    // retryable spelling rather than with a page-store fault naming a page
    // id the client has never heard of.
    CommandDispatcher core1 = DispatcherOn(1);
    Session session;
    const std::string reply = core1.Dispatch("INSERT INTO t VALUES (7)", &session).response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("core 0"), std::string::npos) << reply;
    EXPECT_EQ(reply.find("page"), std::string::npos)
        << "the refusal leaked a storage-layer detail: " << reply;
}

TEST_F(AffinityDispatchTest, AReadOfAnotherCoresRelationSaysWhatIsMissing) {
    CommandDispatcher core0 = DispatcherOn(0);
    Session setup;
    ASSERT_EQ(core0.Dispatch("CREATE TABLE t (id INT64, v INT64)", &setup).response.rfind("ERR", 0),
              std::string::npos);

    CommandDispatcher core1 = DispatcherOn(1);
    Session session;
    const std::string reply = core1.Dispatch("SELECT * FROM t", &session).response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
    EXPECT_NE(reply.find("pipeline"), std::string::npos)
        << "a cross-core read should name what is missing: " << reply;
}

TEST_F(AffinityDispatchTest, ADeleteIsAWriteAndIsCheckedAsOne) {
    CommandDispatcher core0 = DispatcherOn(0);
    Session setup;
    ASSERT_EQ(core0.Dispatch("CREATE TABLE t (id INT64, v INT64)", &setup).response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(core0.Dispatch("INSERT INTO t VALUES (7)", &setup).response.rfind("ERR", 0),
              std::string::npos);

    CommandDispatcher core1 = DispatcherOn(1);
    Session session;
    const std::string reply = core1.Dispatch("DELETE FROM t WHERE id = 1", &session).response;
    EXPECT_EQ(reply.rfind("ERR", 0), 0u) << reply;
}

}  // namespace
}  // namespace kds::server
