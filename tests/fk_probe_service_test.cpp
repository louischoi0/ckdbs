#include "kds/server/fk_probe_service.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// AH-T2, second slice: the foreign key's forward check across owners
// (`instructions/v2.8.0/workorder-ah.md`, `docs/spec/foreign-keys.md` §2a).
//
// Two reactors over one ring, stepped by hand - `statement_ship_service_test`'s
// shape and for its reason (`sched.md` §8's determinism). The child's core is
// 0 and asks; the parent's owner is 1 and answers.
//
// What is pinned here, in the order it would hurt:
//
//   1. **a pass grants an intent, a violation does not.** The intent is the
//      half that closes the validation-to-commit window, and granting one for
//      a row that does not exist would make a parent's DELETE retry forever
//      against a promise nobody is keeping;
//   2. **verdicts are positional**, so a two-parent request is answered
//      about the right two parents - answering one row's question with
//      another's is the failure this protocol must not have;
//   3. **a foreign parent that is not the answering core's is refused**, not
//      answered from, because the child's core resolved the owner from a
//      catalog that can be stale;
//   4. **the decide releases**, and nothing else does.

namespace kds::server {
namespace {

class FkProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/2, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        child_.emplace(clock_, io0_);
        owner_.emplace(clock_, io1_);
        // **The owner is ring core 0 and the child is ring core 1**, not
        // the other way round, because `AssignOwnerCore` puts every
        // relation on the creating core: the parent's `sys.tables` row
        // says 0, so core 0 is who must answer for it. Attaching them the
        // other way made the owner reply to itself, which the ring
        // silently drops - the first draft of this fixture did exactly
        // that and every cell failed on a reply that never came.
        ASSERT_TRUE(owner_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(child_->AttachTransport(&*transport_, 1).ok());

        // The parent relation lives in the owner's store. One database,
        // reached from the owner's side - this rig drives the probe, not
        // ownership, so core 1 simply *is* the owner of what it holds.
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE accounts (id int64, owner varchar) BTREE")
                      .response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO accounts VALUES ('ada')")
                      .response.substr(0, 8),
                  "INSERTED");
        auto oid = boot_->catalog.FindTableOidByName("accounts");
        ASSERT_TRUE(oid.ok());
        accounts_ = oid.value();

        client_.emplace(/*core_id=*/1, *child_, *transport_, clock_);
        ASSERT_TRUE(client_->RegisterReplyReceiver().ok());
    }

    // Installed per test rather than in `SetUp`, because **which core the
    // server believes it is** is the variable one cell needs: the relation
    // says core 0, so a server built as core 5 owns nothing here, which is
    // the migration race in miniature. A handler is registered once, so
    // this may be called once.
    void InstallServer(std::uint32_t core_id) {
        server_.emplace(boot_->catalog, store_, core_id, intents_, *owner_, *transport_);
        ASSERT_TRUE(owner_
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kFkProbeRequest,
                            [this](const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
                                server_->OnRequest(header, payload);
                            })
                        .ok());
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            child_->RunOnce();
            owner_->RunOnce();
        }
    }

    exec::FkParentVerdicts::ForeignGroup GroupOf(
        std::initializer_list<std::pair<catalog::Oid, std::uint64_t>> parents) {
        exec::FkParentVerdicts::ForeignGroup g;
        g.owner_core = 0;
        for (const auto& p : parents) g.parents.push_back(p);
        return g;
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> child_;
    std::optional<sched::Scheduler> owner_;

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
    catalog::Oid accounts_ = 0;

    FkIntentTable intents_;
    std::optional<FkProbeServer> server_;
    std::optional<FkProbeClient> client_;
};

TEST_F(FkProbeTest, APassIsAnsweredAndGrantsAnIntent) {
    InstallServer(/*core_id=*/0);
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/7, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();

    ASSERT_TRUE(client_->Settled(7));
    const FkProbeOutcome* out = client_->Find(7);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived) << "the reply never came back";
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);

    // The half that makes the answer worth having: the parent's owner is
    // now holding that row still for this transaction.
    EXPECT_EQ(intents_.live_rows(), 1u);
    EXPECT_TRUE(intents_.HeldByAnotherThan(accounts_, 1, FkIntentHolder{99, 99}));
}

TEST_F(FkProbeTest, AViolationIsAnsweredAndGrantsNothing) {
    InstallServer(/*core_id=*/0);
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/8, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 999}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(8);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kViolation);

    // **Nothing granted.** An intent on a row that does not exist would be
    // a promise about nothing, and a parent DELETE meeting it would retry
    // forever against it.
    EXPECT_EQ(intents_.live_rows(), 0u);
}

TEST_F(FkProbeTest, VerdictsArePositionalAcrossOneRound) {
    InstallServer(/*core_id=*/0);
    // One round, two parents, opposite answers - AH-R2's whole point is
    // that this is one message, so the mapping back has to be exact.
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/9, /*session_id=*/42,
                              /*transaction_id=*/9,
                              GroupOf({{accounts_, 999}, {accounts_, 1}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(9);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 2u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kViolation) << "the answers came back swapped";
    EXPECT_EQ(out->verdicts[1], exec::FkVerdict::kPass) << "the answers came back swapped";
    EXPECT_EQ(intents_.live_rows(), 1u) << "an intent was granted for the missing parent";
}

TEST_F(FkProbeTest, ADecideReleasesTheIntentAndNothingElseDoes) {
    InstallServer(/*core_id=*/0);
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/10, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();
    ASSERT_EQ(intents_.live_rows(), 1u);

    // Another transaction's decide frees nothing - the holder is the pair
    // (coordinator core, session), and a release keyed loosely would drop
    // a promise still being relied on.
    EXPECT_EQ(server_->ReleaseIntents(/*coordinator_core=*/1, /*session_id=*/43), 0u);
    EXPECT_EQ(intents_.live_rows(), 1u);
    EXPECT_EQ(server_->ReleaseIntents(/*coordinator_core=*/0, /*session_id=*/42), 0u);
    EXPECT_EQ(intents_.live_rows(), 1u);

    // Its own decide frees it, and a resent decide frees nothing more -
    // idempotent, which is what a resendable decide requires.
    EXPECT_EQ(server_->ReleaseIntents(/*coordinator_core=*/1, /*session_id=*/42), 1u);
    EXPECT_EQ(intents_.live_rows(), 0u);
    EXPECT_EQ(server_->ReleaseIntents(/*coordinator_core=*/1, /*session_id=*/42), 0u);
}

TEST_F(FkProbeTest, AParentThisCoreDoesNotOwnIsRefusedRatherThanAnswered) {
    InstallServer(/*core_id=*/0);

    // A relation whose `sys.tables` row says **core 1**, placed there by a
    // second catalog over the same store under `kRotate` - the one way this
    // engine can currently produce a relation a given core does not own
    // (`AssignOwnerCore` otherwise puts every relation on its creator).
    //
    // Asking core 0 about it is the migration race in miniature: the child's
    // core resolved an owner from a catalog that had since moved on, and the
    // core it asked must say so rather than answer from a relation that is
    // not its own.
    catalog::Catalog rotated(store_, storage::kDefaultInlineCellWidth, /*core_count=*/2,
                             /*core_id=*/0);
    rotated.SetPlacementPolicy(catalog::PlacementPolicy::kRotate);
    catalog::Schema schema;
    catalog::SysColumnRow id{};
    id.pos = 0;
    catalog::SetName(id.name, "id");
    id.type_val = catalog::kTypeValInt64;
    id.len = 8;
    id.notnull = true;
    schema.columns.push_back(id);
    auto elsewhere = rotated.CreateTable(catalog::kNamespacePublic, "elsewhere", schema,
                                         catalog::ClusteredType::kBtree);
    ASSERT_TRUE(elsewhere.ok()) << elsewhere.status().message();
    auto row = boot_->catalog.GetSysTableRow(elsewhere.value());
    ASSERT_TRUE(row.ok());
    ASSERT_NE(row.value().owner_core, 0u) << "the fixture failed to place a relation off core 0";

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/11, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{elsewhere.value(), 1}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(11);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_FALSE(out->status.ok()) << "a core that owns nothing answered about a parent";
    // Retryable, because the owner moving is not the statement being wrong.
    EXPECT_EQ(out->status.code(), StatusCode::kTxnConflict) << out->status.message();
    EXPECT_TRUE(out->verdicts.empty());
    EXPECT_EQ(intents_.live_rows(), 0u);
}

TEST_F(FkProbeTest, AGroupPastTheCapRefusesAndOpensNoWaiter) {
    exec::FkParentVerdicts::ForeignGroup big;
    big.owner_core = 0;
    for (std::size_t k = 0; k <= kFkProbeMaxParents; ++k) {
        big.parents.emplace_back(accounts_, static_cast<std::uint64_t>(k + 1));
    }

    Status refused = client_->Request(/*owner_core=*/0, /*request_id=*/12, /*session_id=*/42,
                                      /*transaction_id=*/9, big);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kNotImplemented) << refused.message();
    // No waiter, so nothing parks on an answer that will never come.
    EXPECT_EQ(client_->waiting(), 0u);
    EXPECT_EQ(client_->Find(12), nullptr);
}

TEST_F(FkProbeTest, ADeadlineSettlesTheWaiterWithNoReply) {
    // Nothing pumps the owner, so no reply is possible.
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/13, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    EXPECT_FALSE(client_->Settled(13));

    clock_.Advance(kFkProbeReplyDeadlineNs);
    EXPECT_TRUE(client_->Settled(13));
    const FkProbeOutcome* out = client_->Find(13);
    ASSERT_NE(out, nullptr);
    // Settled without having arrived is the deadline, and the caller has to
    // be able to tell the two apart: a verdict was never given, so no row
    // may be written on the strength of one.
    EXPECT_FALSE(out->arrived);
}

}  // namespace
}  // namespace kds::server
