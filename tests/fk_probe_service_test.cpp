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
    // `cabins` non-null is core 0's configuration: `Expeditor` holds a
    // `cabin_store_` and `CoreRuntime` passes a peer's dispatcher none, so
    // the Cabin arm of the reverse check exists on one core and not the
    // others. Both are worth a fixture.
    void InstallServer(std::uint32_t core_id, stats::CabinStore* cabins = nullptr) {
        server_.emplace(boot_->catalog, store_, core_id, intents_, pending_deletes_, *owner_,
                        *transport_, /*txn=*/nullptr, /*log=*/nullptr, cabins);
        ASSERT_TRUE(owner_
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kFkProbeRequest,
                            [this](const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
                                server_->OnRequest(header, payload);
                            })
                        .ok());
    }

    // AJ-T2's handler, on the same server the forward's is. Separate from
    // `InstallServer` because a handler registers once and some cells want
    // only one direction armed.
    void InstallReverseHandler() {
        ASSERT_TRUE(owner_
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kFkReverseProbeRequest,
                            [this](const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
                                server_->OnReverseRequest(header, payload);
                            })
                        .ok());
    }

    exec::FkReverseProbeGroup ReverseGroupOf(
        std::initializer_list<exec::FkReverseProbeEntry> entries) {
        exec::FkReverseProbeGroup g;
        g.owner_core = 0;
        for (const auto& e : entries) g.entries.push_back(e);
        return g;
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            child_->RunOnce();
            owner_->RunOnce();
        }
    }

    // A relation whose `sys.tables` row says a core other than 0, placed
    // there by a second catalog over the same store under `kRotate` - the
    // one way this engine can currently produce a relation a given core
    // does not own (`AssignOwnerCore` otherwise puts every relation on its
    // creator). Asking core 0 about it is the migration race in miniature:
    // the child's core resolved an owner from a catalog that had since
    // moved on, and the core it asked must say so rather than answer from a
    // relation that is not its own.
    catalog::Oid MakeRelationOffCore0(const char* name) {
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
        auto oid = rotated.CreateTable(catalog::kNamespacePublic, name, schema,
                                       catalog::ClusteredType::kBtree);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        if (!oid.ok()) return 0;
        auto row = boot_->catalog.GetSysTableRow(oid.value());
        EXPECT_TRUE(row.ok());
        EXPECT_NE(row.value().owner_core, 0u)
            << "the fixture failed to place a relation off core 0";
        return oid.value();
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
    FkPendingDeleteTable pending_deletes_;
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

    const catalog::Oid elsewhere = MakeRelationOffCore0("elsewhere");
    ASSERT_NE(elsewhere, 0u);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/11, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{elsewhere, 1}}))
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


// ---- AH-T4: the reverse direction, now that F5 is converted -------------
//
// Lifting the declaration refusal makes two things live that the refusal
// was holding shut, and both are fail-closed rather than answered:
//
//   1. a **parent DELETE meeting a foreign intent** answers busy, because
//      the row it would remove is one another core was told exists and is
//      now writing a child against;
//   2. a **reverse check over a child this core does not own** refuses,
//      because RESTRICT needs an authoritative "no children" and this core
//      cannot see them. `workplan-auxiliaries-under-split.md` §3.1 named
//      this exposure and said the day F5 relaxes, both directions are
//      owed - this is the owner direction being paid.

TEST_F(FkProbeTest, AGrantedIntentIsWhatAParentDeleteWouldHaveToMeet) {
    InstallServer(/*core_id=*/0);
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/20, /*session_id=*/42,
                              /*transaction_id=*/0, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();
    ASSERT_EQ(intents_.live_rows(), 1u);

    // The predicate the parent-side DELETE asks, with the holder that core
    // would pass: its own, which matches nothing here because an intent on
    // this table is always a foreign core's.
    EXPECT_TRUE(intents_.HeldByAnotherThan(accounts_, 1, FkIntentHolder{0, 0}))
        << "a granted intent is invisible to the delete that must respect it";
    // And it is scoped to the row, not the relation: deleting any other
    // parent row is unaffected.
    EXPECT_FALSE(intents_.HeldByAnotherThan(accounts_, 2, FkIntentHolder{0, 0}));
}

TEST_F(FkProbeTest, AReverseCheckOverAChildAnotherCoreOwnsRefuses) {
    // A child relation placed off core 0 by the same rotate device the
    // stale-owner cell uses.
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
    catalog::SysColumnRow ref{};
    ref.pos = 1;
    catalog::SetName(ref.name, "account_id");
    ref.type_val = catalog::kTypeValInt64;
    ref.len = 8;
    ref.notnull = true;
    schema.columns.push_back(ref);
    auto child = rotated.CreateTable(catalog::kNamespacePublic, "foreign_trades", schema,
                                     catalog::ClusteredType::kBtree);
    ASSERT_TRUE(child.ok()) << child.status().message();

    auto access = boot_->catalog.InitTableAccess(child.value());
    ASSERT_TRUE(access.ok());
    ASSERT_NE(access.value()->owner_core, 0u) << "the fixture failed to place a child off core 0";

    exec::FkReverseOptions options;
    options.core_id = 0;
    exec::Budget budget;
    auto outcome = exec::CheckNoChildReferences(store_, *access.value(), /*child_column_no=*/1,
                                                /*parent_pk=*/1, txn::ReadView::Everything(),
                                                options, &budget);
    EXPECT_FALSE(outcome.ok())
        << "a reverse check walked a child relation this core does not own";
    // `NotImplemented`, by the two-code rule: the architecture admits this
    // - the fan-out is specified - and nobody built the sender.
    EXPECT_EQ(outcome.status().code(), StatusCode::kNotImplemented) << outcome.status().message();
    EXPECT_NE(outcome.status().message().find("owned by core"), std::string::npos)
        << outcome.status().message();
}

// ---- AJ-T1: the pending-delete set's consult ------------------------------
//
// The mirror of the intent above. These cells drive the table directly
// rather than through a DELETE, because the statement that registers is
// AJ-T3's and the consult is this task's - and a consult that only works
// once its one caller exists is a consult nobody can regress.

TEST_F(FkProbeTest, ARowThisCoreIsAboutToDeleteIsAnsweredBusy) {
    InstallServer(/*core_id=*/0);
    // The registration a DELETE makes at its dispatch fork, before it fans
    // out to the child's owner. Row 1 exists and would otherwise pass -
    // `APassIsAnsweredAndGrantsAnIntent` is the same request without this
    // line.
    //
    // **Registered under the same session id the probe below carries**, and
    // deliberately: the two numbers come from different cores' counters,
    // both minted from 1, so this collision is the *common* case rather
    // than a contrived one. A consult that excluded the asker's own id
    // would answer "not pending" here and grant an intent on a row on its
    // way out - which is why `FkPendingDeleteTable::Pending` takes no
    // session at all. A registration under some other id would let that
    // bug through unnoticed.
    pending_deletes_.Add(accounts_, /*parent_pk=*/1, /*session_id=*/42);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/11, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(11);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    // Busy, not violation: the delete has not committed, so the answer
    // depends on how it ends and the child retries. F3.
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kBusy);

    // **And nothing was granted**, which is the whole point. An intent here
    // would be a promise about a row on its way out - the child would
    // commit on the strength of it and the DELETE's per-row check, which
    // has already been told "no children", would mark the row anyway.
    EXPECT_EQ(intents_.live_rows(), 0u);
    EXPECT_EQ(pending_deletes_.stats().refusals, 1u);
}

TEST_F(FkProbeTest, APendingDeleteOnARowThatDoesNotExistStillAnswersBusy) {
    // A wire-visible consequence of putting the consult ahead of the
    // existence read, pinned rather than discovered. Row 999 is not there,
    // so `AViolationIsAnsweredAndGrantsNothing` gets the terminal
    // `kViolation` for it - but while a DELETE has registered that pk the
    // answer is the retryable `kBusy` instead.
    //
    // **Correct, and self-resolving.** The registration ends with the
    // deleting statement, after which the same probe answers `kViolation`
    // again; and the ordering cannot be reversed, because reading first is
    // exactly the window that lets a pass be computed for a row already on
    // its way out. What a client sees is a retry that then gets the real
    // answer, never a wrong one.
    InstallServer(/*core_id=*/0);
    pending_deletes_.Add(accounts_, /*parent_pk=*/999, /*session_id=*/7);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/16, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 999}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(16);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kBusy);
    EXPECT_EQ(intents_.live_rows(), 0u);
}

TEST_F(FkProbeTest, APendingDeleteOnAnotherRowDoesNotAffectThisOne) {
    InstallServer(/*core_id=*/0);
    // The negative half, and it is worth a cell of its own: a table keyed
    // on the relation alone would refuse every probe against `accounts`
    // while any of its rows was being deleted, which is a correctness-safe
    // answer and a uselessly coarse one.
    pending_deletes_.Add(accounts_, /*parent_pk=*/999, /*session_id=*/7);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/12, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(12);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);
    EXPECT_EQ(intents_.live_rows(), 1u);
    EXPECT_EQ(pending_deletes_.stats().refusals, 0u);
}

TEST_F(FkProbeTest, AClearedPendingDeleteLetsTheNextProbePass) {
    InstallServer(/*core_id=*/0);
    pending_deletes_.Add(accounts_, /*parent_pk=*/1, /*session_id=*/7);
    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/13, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();
    ASSERT_EQ(client_->Find(13)->verdicts[0], exec::FkVerdict::kBusy);
    client_->Close(13);

    // What the deleting session's `COMMIT` or `ROLLBACK` does. The row is
    // referenceable again immediately - a busy answer that outlived its
    // cause is the "retry loop that cannot succeed" F1 already paid for
    // once from the other direction.
    EXPECT_EQ(pending_deletes_.Release(/*session_id=*/7), 1u);
    EXPECT_EQ(pending_deletes_.live_rows(), 0u);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/14, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    Pump();
    const FkProbeOutcome* out = client_->Find(14);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);
    EXPECT_EQ(intents_.live_rows(), 1u);
}

TEST_F(FkProbeTest, TheOwnershipReCheckStillWinsOverAPendingDelete) {
    // Ordering, pinned from outside. A pending delete must not turn the
    // "not this core's relation" refusal into a busy verdict: busy tells
    // the child to retry, and retrying a relation this core does not own is
    // a condition that never clears here. The refusal has to survive.
    //
    // A server built as some other core could not reply at all - the
    // transport has only the two cores this rig wires - so the relation is
    // what moves, not the server.
    InstallServer(/*core_id=*/0);
    const catalog::Oid elsewhere = MakeRelationOffCore0("elsewhere_pending");
    ASSERT_NE(elsewhere, 0u);

    // A registration on the very row the probe names. It must change
    // nothing: this core has no business answering about that relation at
    // all, however much it believes it is deleting one of its rows.
    pending_deletes_.Add(elsewhere, /*parent_pk=*/1, /*session_id=*/7);

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/15, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{elsewhere, 1}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(15);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_FALSE(out->status.ok()) << "a probe answered from a relation this core does not own";
    EXPECT_EQ(out->status.code(), StatusCode::kTxnConflict) << out->status.message();
    EXPECT_TRUE(out->verdicts.empty()) << "a busy verdict displaced the ownership refusal";
    EXPECT_EQ(pending_deletes_.stats().refusals, 0u);
}

// ---- AJ-T2: the reverse pair ---------------------------------------------
//
// The other direction of the same crossing. The forward asks the parent's
// owner *"does this row exist"*; this asks the child's owner *"does anything
// still reference it"*, which is what RESTRICT needs before a parent row may
// be deleted. The sender is AJ-T3's; what is pinned here is the wire, the
// handler and what it does **not** leave behind.

class FkReverseProbeTest : public FkProbeTest {
protected:
    void SetUp() override {
        FkProbeTest::SetUp();
        ASSERT_EQ(dispatcher_
                      ->Dispatch("CREATE TABLE trades (id int64, account_id int64 "
                                 "REFERENCES accounts) BTREE")
                      .response.substr(0, 7),
                  "CREATED");
        auto oid = boot_->catalog.FindTableOidByName("trades");
        ASSERT_TRUE(oid.ok());
        trades_ = oid.value();
    }

    // `trades.account_id` is column 1: column 0 is the Keystone id.
    static constexpr std::uint16_t kAccountIdColumn = 1;

    catalog::Oid trades_ = 0;
};

TEST_F(FkReverseProbeTest, TheWireCarriesBothKindsAndTheyFitOneSlot) {
    // The discipline `txn_2pc_wire_test` sets for its own four: a kind the
    // enum carries but `IsKnownRingMessageKind` does not is **dropped on
    // arrival**, and from the sender's side that is indistinguishable from
    // a peer that never answers - here it would cost a full five-second
    // probe deadline and a refusal naming a condition that cannot clear.
    const sched::RingMessageKind kinds[] = {
        sched::RingMessageKind::kFkReverseProbeRequest,
        sched::RingMessageKind::kFkReverseProbeReply,
    };
    for (const sched::RingMessageKind kind : kinds) {
        EXPECT_TRUE(sched::IsKnownRingMessageKind(static_cast<std::uint16_t>(kind)))
            << sched::RingMessageKindName(kind);
        EXPECT_STRNE(sched::RingMessageKindName(kind), "unknown");
    }
    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kFkReverseProbeRequest),
                 "FK_REVERSE_PROBE_REQUEST");
    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kFkReverseProbeReply),
                 "FK_REVERSE_PROBE_REPLY");

    // The cap follows the types rather than a paragraph. Stated as an
    // equality so that widening `child_column_no` moves this line too,
    // instead of silently spending the headroom - the failure a chosen 64
    // cost the forward pair once.
    EXPECT_EQ(kFkReverseProbeMaxEntries,
              (sched::kCoreRingPayloadBytes - kFkReverseProbeRequestHeadBytes) /
                  (2 * sizeof(std::uint64_t) + sizeof(std::uint16_t)));
    EXPECT_LE(sizeof(FkReverseProbeRequestPayload), sched::kCoreRingPayloadBytes);
    EXPECT_LE(sizeof(FkReverseProbeReplyPayload), sched::kCoreRingPayloadBytes);
}

TEST_F(FkReverseProbeTest, NoChildIsAnsweredClearAndLeavesNothingBehind) {
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();
    // `trades` is empty, so nothing references account 1.
    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/20, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(20);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived) << "the reverse reply never came back";
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);

    // **AJ-R5, and this is the assertion the whole design turns on.** The
    // forward's equivalent cell asserts an intent was granted; this one
    // asserts nothing was. A reverse answer enrols nobody, so there is no
    // decide target, no release leg, and none of the 720x an
    // intent-holding participant pays.
    EXPECT_EQ(intents_.live_rows(), 0u);
    EXPECT_EQ(pending_deletes_.live_rows(), 0u);
}

TEST_F(FkReverseProbeTest, ACommittedChildIsAnsweredViolation) {
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (1)").response.substr(0, 8),
              "INSERTED");

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/21, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(21);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    // Terminal, not retryable: the child is committed and visible, so
    // re-running the DELETE violates the constraint again.
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kViolation);
    EXPECT_EQ(intents_.live_rows(), 0u);
}

TEST_F(FkReverseProbeTest, VerdictsArePositionalAcrossOneReverseRound) {
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (1)").response.substr(0, 8),
              "INSERTED");

    // One round, two questions, opposite answers - AH-R2's deduplication
    // applied to this direction, so the mapping back has to be exact.
    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/22, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 999, kAccountIdColumn},
                                                     {trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(22);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 2u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass) << "nothing references account 999";
    EXPECT_EQ(out->verdicts[1], exec::FkVerdict::kViolation) << "trade 1 references account 1";
}

TEST_F(FkReverseProbeTest, AChildThisCoreDoesNotOwnIsRefusedRatherThanAnswered) {
    // The mirror of the forward's ownership re-check, and the reason it has
    // to exist: the *deleting* core resolved this owner from its own
    // catalog, which can be stale. Answering "no children" from a relation
    // this core cannot see is the dangling reference the fan-out exists to
    // prevent, one hop further along - and it is the one wrong answer that
    // would be silent, because "clear" is what lets the DELETE proceed.
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();
    const catalog::Oid elsewhere = MakeRelationOffCore0("elsewhere_child");
    ASSERT_NE(elsewhere, 0u);

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/23, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{elsewhere, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(23);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_FALSE(out->status.ok()) << "a core that owns nothing answered 'no children'";
    // Retryable, because the owner moving is not the statement being wrong.
    EXPECT_EQ(out->status.code(), StatusCode::kTxnConflict) << out->status.message();
    EXPECT_TRUE(out->verdicts.empty());
}

TEST_F(FkReverseProbeTest, AGroupPastTheCapOpensNoWaiterAndRefuses) {
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();
    exec::FkReverseProbeGroup group;
    group.owner_core = 0;
    for (std::size_t i = 0; i <= kFkReverseProbeMaxEntries; ++i) {
        group.entries.push_back({trades_, i + 1, kAccountIdColumn});
    }

    const Status sent = client_->RequestReverse(/*owner_core=*/0, /*request_id=*/24,
                                                /*session_id=*/42, /*transaction_id=*/9, group);
    EXPECT_FALSE(sent.ok());
    // `NotImplemented` by the two-code rule: chunking is a thing nobody
    // built, not a thing the architecture refuses.
    EXPECT_EQ(sent.code(), StatusCode::kNotImplemented) << sent.message();
    // **No waiter opened**, which is what makes the refusal safe: a
    // statement that cannot phrase its question does not park on an answer
    // that will never come.
    EXPECT_EQ(client_->Find(24), nullptr);
}

TEST_F(FkReverseProbeTest, BothDirectionsShareOneWaiterMapWithoutColliding) {
    // S2's design claim, pinned: the two kinds land in the same map, keyed
    // by request id, so one park can cover a mixed set. A statement never
    // sends both today - a DELETE runs no forward check - but the client is
    // one object and the collision would be silent if the ids ever met.
    InstallServer(/*core_id=*/0);
    InstallReverseHandler();

    ASSERT_TRUE(client_
                    ->Request(/*owner_core=*/0, /*request_id=*/25, /*session_id=*/42,
                              /*transaction_id=*/9, GroupOf({{accounts_, 1}}))
                    .ok());
    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/26, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* forward = client_->Find(25);
    const FkProbeOutcome* reverse = client_->Find(26);
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(reverse, nullptr);
    ASSERT_TRUE(forward->arrived);
    ASSERT_TRUE(reverse->arrived);
    ASSERT_EQ(forward->verdicts.size(), 1u);
    ASSERT_EQ(reverse->verdicts.size(), 1u);
    // The forward found the row and took an intent for it; the reverse
    // found no children and took nothing. One map, two answers, and the
    // counters say which direction each round went.
    EXPECT_EQ(forward->verdicts[0], exec::FkVerdict::kPass);
    EXPECT_EQ(reverse->verdicts[0], exec::FkVerdict::kPass);
    EXPECT_EQ(intents_.live_rows(), 1u);
    EXPECT_EQ(client_->requests(), 1u);
    EXPECT_EQ(client_->reverse_requests(), 1u);
}

// ---- AJ-T2: the Cabin-served reverse answer (F6) --------------------------
//
// **The highest-risk path in the pair**, and the reason it gets its own
// fixture. Everything else here fails loudly: a wrong ownership check
// refuses, a wrong cap refuses. This one fails *quietly* - F6 says a
// verified-empty entry set is an **authoritative** "no children", so a
// defect on this branch is a wrong `kPass`, which is a dropped constraint
// and a dangling reference rather than an error anybody sees.
//
// `Stats::hits` against `Stats::misses` is what says which path answered.
// Nothing is banked that disagrees with the relation - a set that lied
// would pin behaviour under a precondition §6a forbids - so the
// discriminator is the counter, not a rigged answer.
class FkReverseCabinTest : public FkProbeTest {
protected:
    void SetUp() override {
        FkProbeTest::SetUp();
        ASSERT_EQ(dispatcher_
                      ->Dispatch("CREATE TABLE trades (id int64, account_id int64 "
                                 "REFERENCES accounts CABIN) BTREE")
                      .response.substr(0, 7),
                  "CREATED");
        auto oid = boot_->catalog.FindTableOidByName("trades");
        ASSERT_TRUE(oid.ok());
        trades_ = oid.value();

        auto access = boot_->catalog.InitTableAccess(trades_);
        ASSERT_TRUE(access.ok());
        cabin_id_ = access.value()->CabinOn(kAccountIdColumn).id;
        ASSERT_NE(cabin_id_, 0u) << "the CABIN clause did not nominate a cabin on account_id";
    }

    std::optional<stats::CabinKey> KeyFor(std::uint64_t parent_pk) {
        parser::AstValue v;
        v.type = parser::ValueType::kInt;
        v.int_val = static_cast<std::int64_t>(parent_pk);
        return stats::MakeCabinKey(cabin_id_, v);
    }

    static constexpr std::uint16_t kAccountIdColumn = 1;

    stats::CabinStore cabins_;
    catalog::Oid trades_ = 0;
    std::uint64_t cabin_id_ = 0;
};

TEST_F(FkReverseCabinTest, AnObservedEmptySetIsAnAuthoritativeNoChildren) {
    InstallServer(/*core_id=*/0, &cabins_);
    InstallReverseHandler();

    // `trades` genuinely has no rows, so the banked set is true as well as
    // empty - which is what §6a requires of anything banked at all.
    auto key = KeyFor(1);
    ASSERT_TRUE(key.has_value());
    ASSERT_TRUE(cabins_.Commit(*key, {}));

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/30, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(30);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived) << "the reverse reply never came back";
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);

    // **The assertion that makes this cell mean anything.** Without it the
    // test passes against a handler that never consults the Cabin at all -
    // the walk of an empty relation answers `kPass` too. A hit and no miss
    // is the store saying it served the answer.
    EXPECT_EQ(cabins_.stats().hits, 1u) << "the reverse check did not consult the Cabin";
    EXPECT_EQ(cabins_.stats().misses, 0u);
}

TEST_F(FkReverseCabinTest, AnObservedSetCarryingALiveChildAnswersViolation) {
    InstallServer(/*core_id=*/0, &cabins_);
    InstallReverseHandler();
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (1)").response.substr(0, 8),
              "INSERTED");

    // **Banked by hand, and it has to be**: in production the write hook
    // fills a set through the *dispatcher's* Cabin store, and this fixture's
    // dispatcher has none - `cabins_` belongs to the probe server, which is
    // exactly the split core 0 has. So the set is constructed as the hook
    // would leave it: the real child's pk, with no hint, which sends the
    // check through the btree descent that heals it. Nothing here disagrees
    // with the relation, which is what §6a requires of anything banked.
    auto key = KeyFor(1);
    ASSERT_TRUE(key.has_value());
    stats::CabinEntry entry;
    entry.pk = 1;  // trade 1, the row the INSERT above placed
    ASSERT_TRUE(cabins_.Commit(*key, {entry}));

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/31, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(31);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_TRUE(out->status.ok()) << out->status.message();
    ASSERT_EQ(out->verdicts.size(), 1u);
    // The set is a **superset**, so every entry is re-checked against the
    // row and the key: a Cabin never turns a violation into a pass, it only
    // saves the walk that would have found it.
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kViolation);
    EXPECT_EQ(cabins_.stats().hits, 1u);
}

TEST_F(FkReverseCabinTest, AnUnobservedValueFallsThroughToTheWalk) {
    InstallServer(/*core_id=*/0, &cabins_);
    InstallReverseHandler();

    // Nothing banked for account 7. `Find` returning null is not "no
    // children" - it is "this Cabin knows nothing about that value" - and
    // conflating the two is the wrong `kPass` this whole fixture guards.
    auto key = KeyFor(7);
    ASSERT_TRUE(key.has_value());
    ASSERT_EQ(cabins_.Find(*key), nullptr);

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/32, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 7, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(32);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);
    // A miss, not a hit: the answer came from the walk.
    EXPECT_EQ(cabins_.stats().hits, 0u);
    EXPECT_EQ(cabins_.stats().misses, 1u);
}

TEST_F(FkReverseCabinTest, WithNoCabinStoreTheAnswerIsTheSameAndTheWalkDoesIt) {
    // A **peer's** configuration, which is every core but 0:
    // `CoreRuntime` passes `/*cabins=*/nullptr`. The answer must be
    // identical - a Cabin is advisory - and the only difference is that
    // nothing was consulted.
    InstallServer(/*core_id=*/0, /*cabins=*/nullptr);
    InstallReverseHandler();
    auto key = KeyFor(1);
    ASSERT_TRUE(key.has_value());
    ASSERT_TRUE(cabins_.Commit(*key, {}));

    ASSERT_TRUE(client_
                    ->RequestReverse(/*owner_core=*/0, /*request_id=*/33, /*session_id=*/42,
                                     /*transaction_id=*/9,
                                     ReverseGroupOf({{trades_, 1, kAccountIdColumn}}))
                    .ok());
    Pump();

    const FkProbeOutcome* out = client_->Find(33);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    ASSERT_EQ(out->verdicts.size(), 1u);
    EXPECT_EQ(out->verdicts[0], exec::FkVerdict::kPass);
    EXPECT_EQ(cabins_.stats().hits, 0u) << "a server given no store consulted one";
}

}  // namespace
}  // namespace kds::server
