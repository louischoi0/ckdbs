// **Another core's transaction is in flight until it decides** (AX-S1,
// `instructions/v3.0.0/workorder-ax-inflight-publication.md`), on the
// two-core rig.
//
// Before AX-S1 `TransactionManager::IsInFlight` walked its own core's live
// list, so core 1 answered "not in flight" for core 0's open transaction
// from its first statement - and, after a rollback, had nothing to tell the
// loser from a running one either, since an abort leaves no window entry.
// Now each core publishes its running ids into the instance's in-flight
// tables and every core reads all of them.
//
// Driven synchronously from the test thread before the reactors start, as
// `catalog_name_rig_test.cpp` is: the question is what one core's manager
// answers about another core's transaction, not an interleaving - that one
// is `instance_visibility_test.cpp`'s swap-remove cell.

#include "two_core_rig.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

namespace kds::server {
namespace {

std::unique_ptr<TwoCoreRig> OpenRig() {
    auto opened = TwoCoreRig::Open(TwoCoreRig::Options{});
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

bool StartsWith(const std::string& reply, std::string_view prefix) {
    return reply.rfind(prefix, 0) == 0;
}

// Core 0 opens a transaction and writes one row in it; the id it runs
// under, or 0 when the fixture failed.
std::uint64_t OpenAWrite(TwoCoreRig& rig, Session& session) {
    CommandDispatcher& d0 = rig.core(0).dispatcher();
    EXPECT_TRUE(StartsWith(d0.Dispatch("CREATE TABLE t (id int64, v int64)").response, "CREATED"));
    EXPECT_TRUE(StartsWith(d0.Dispatch("BEGIN", &session).response, "BEGIN"));
    const std::string wrote = d0.Dispatch("INSERT INTO t VALUES (1, 10)", &session).response;
    EXPECT_FALSE(StartsWith(wrote, "ERR")) << wrote;
    return session.transaction() != nullptr ? session.transaction()->id() : 0;
}

TEST(InflightPublicationRigTest, APeerAnswersInFlightForAnotherCoresOpenTransactionUntilCommit) {
    // **Mutation**: `IsInFlight` walking `live_` again - the first
    // expectation fails, as it did before AX-S1.
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Session session;
    const std::uint64_t id = OpenAWrite(*rig, session);
    ASSERT_NE(id, 0u);

    EXPECT_TRUE(rig->core(1).transactions().IsInFlight(id))
        << "core 1 answered not-in-flight for core 0's open transaction " << id;
    ASSERT_TRUE(StartsWith(rig->core(0).dispatcher().Dispatch("COMMIT", &session).response,
                           "COMMIT"));
    EXPECT_FALSE(rig->core(1).transactions().IsInFlight(id));
    EXPECT_FALSE(rig->core(0).transactions().IsInFlight(id));
}

TEST(InflightPublicationRigTest, APeerAnswersInFlightForAnotherCoresOpenTransactionUntilRollback) {
    auto rig = OpenRig();
    ASSERT_NE(rig, nullptr);
    Session session;
    const std::uint64_t id = OpenAWrite(*rig, session);
    ASSERT_NE(id, 0u);

    EXPECT_TRUE(rig->core(1).transactions().IsInFlight(id));
    ASSERT_TRUE(StartsWith(rig->core(0).dispatcher().Dispatch("ROLLBACK", &session).response,
                           "ROLLBACK"));
    EXPECT_FALSE(rig->core(1).transactions().IsInFlight(id))
        << "a rolled-back transaction still reads as in flight on the peer";
    EXPECT_FALSE(rig->core(0).transactions().IsInFlight(id));
}

}  // namespace
}  // namespace kds::server
