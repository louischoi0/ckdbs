#include "kds/server/statement_ship_service.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/server/command_dispatcher.hpp"

// SS1: the shipped-statement wire and the waiter on it.
//
// Two reactors over one ring, stepped by hand, which is what makes every
// one of these deterministic (`docs/spec/sched.md` §8's simulation shape, and
// the shape `trx_id_lease_service_test.cpp` already uses).
//
// The three things worth pinning here, in the order they would hurt:
//
//   1. a deadline is **not** a refusal. It answers `UnknownOutcome`, which
//      no retry loop follows, because the statement may have committed;
//   2. a reply is matched to its waiter by (session, sequence) and not by
//      the ring's request id alone - answering one statement with
//      another's result is the failure this protocol must not have;
//   3. the caps refuse rather than truncate, both ways.

namespace kds::server {
namespace {

class StatementShipTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/2, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        arrival_.emplace(clock_, io0_);
        owner_.emplace(clock_, io1_);
        ASSERT_TRUE(arrival_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(owner_->AttachTransport(&*transport_, 1).ok());

        client_.emplace(/*core_id=*/0, *arrival_, *transport_, clock_);
        ASSERT_TRUE(client_->RegisterReplyReceiver().ok());
    }

    // Installs the owner with an executor, and registers its handler. Taken
    // as a parameter rather than fixed because what SS3 will put here is
    // the whole variable: these tests drive the seam, not the execution.
    void InstallOwner(StatementShipServer::ExecuteFn execute) {
        server_.emplace(/*core_id=*/1, *owner_, *transport_, std::move(execute));
        ASSERT_TRUE(owner_
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kShippedStatementRequest,
                            [this](const sched::MessageHeader& header,
                                   std::span<const std::byte> payload) {
                                server_->OnRequest(header, payload);
                            })
                        .ok());
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            arrival_->RunOnce();
            owner_->RunOnce();
        }
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> arrival_;
    std::optional<sched::Scheduler> owner_;
    std::optional<StatementShipClient> client_;
    std::optional<StatementShipServer> server_;
};

TEST_F(StatementShipTest, AStatementCrossesAndItsAnswerComesBack) {
    std::vector<std::string> seen;
    std::uint64_t seen_session = 0, seen_sequence = 0, seen_oid = 0;
    Role seen_role = Role::kAdmin;
    std::uint32_t seen_requester = 99;
    InstallOwner([&](StatementShipServer::ShippedStatement st,
                     StatementShipServer::ReplyFn reply) {
        seen_requester = st.requester;
        seen_session = st.session_id;
        seen_sequence = st.sequence;
        seen_oid = st.target_oid;
        seen_role = st.role;
        seen.emplace_back(std::move(st.text));
        reply(Status::OK(), "INSERTED oid=4000 id=7 page=12 slot=3");
    });

    ASSERT_TRUE(client_
                    ->Ship(/*owner_core=*/1, /*request_id=*/1, /*session_id=*/99,
                           /*sequence=*/4, /*target_oid=*/4000, Role::kReadWrite,
                           "INSERT INTO t VALUES ('a', 1)")
                    .ok());
    EXPECT_EQ(client_->waiting(), 1u);
    EXPECT_FALSE(client_->Settled(1));

    Pump();

    ASSERT_TRUE(client_->Settled(1));
    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->arrived);
    EXPECT_TRUE(out->status.ok()) << out->status.message();
    EXPECT_EQ(out->text, "INSERTED oid=4000 id=7 page=12 slot=3");

    // D4's identity survived the round trip unchanged, which is what the
    // owner's dedup record (SS3) will key on.
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "INSERT INTO t VALUES ('a', 1)");
    EXPECT_EQ(seen_session, 99u);
    EXPECT_EQ(seen_sequence, 4u);
    EXPECT_EQ(seen_oid, 4000u);
    EXPECT_EQ(server_->requests(), 1u);
    EXPECT_EQ(server_->replies(), 1u);
    EXPECT_EQ(client_->late_executed_replies(), 0u);
}

TEST_F(StatementShipTest, TheOwnersRefusalArrivesAsTheOwnerSpelledIt) {
    // Including the retryable bit, which is the one bit clients build retry
    // loops on (docs/spec/protocol.md §11) and the thing a re-wrapped status
    // silently loses.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) {
        reply(Status::TxnConflict("extent lease: this core's lease of 64 pages is spent"), {});
    });

    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());
    Pump();

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_EQ(out->status.code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(out->status.retryable());
    EXPECT_NE(out->status.message().find("lease of 64 pages is spent"), std::string::npos)
        << out->status.message();
    // A refusal's message lives in the status; `text` is the success arm's
    // and must not double as a copy of it.
    EXPECT_TRUE(out->text.empty());
}

TEST_F(StatementShipTest, ADeadlineIsUnknownOutcomeAndIsNotRetryable) {
    // **The one that matters.** A timeout is not a refusal: the statement
    // may have committed on the owner, so anything a client would retry
    // invites a second row under a second engine-issued pk. Nothing is
    // installed on the owner, so nothing ever answers.
    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());
    Pump();
    EXPECT_FALSE(client_->Settled(1)) << "nothing answered, so the wait must still be open";

    clock_.Advance(kShippedStatementDeadlineNs);
    EXPECT_TRUE(client_->Settled(1));

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    EXPECT_FALSE(out->arrived) << "arrived=false after Settled is what names the deadline";

    // The caller turns that into the status; this is the contract it rests
    // on, asserted here so the two cannot drift.
    EXPECT_FALSE(IsRetryable(StatusCode::kUnknownOutcome));
    EXPECT_FALSE(Status::UnknownOutcome("x").retryable());
}

TEST_F(StatementShipTest, AnUninstalledExecutorRefusesRatherThanLookingLikeACrash) {
    // SS1 builds the wire; SS3 fills the seam. A request that arrives at a
    // core with no executor must answer, or the arrival core pays a full
    // deadline to learn that nothing is built yet.
    InstallOwner({});
    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());
    Pump();

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_EQ(out->status.code(), StatusCode::kUnsupported);
    EXPECT_NE(out->status.message().find("SS3"), std::string::npos) << out->status.message();
}

TEST_F(StatementShipTest, AReplyWhoseIdentityDoesNotMatchItsWaiterIsDropped) {
    // Answering one statement with another's result is the failure this
    // protocol must not have. The waiter is left to its deadline, which
    // says "unknown outcome" - truthful, because this core now knows
    // nothing about its own statement.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    ASSERT_TRUE(client_->Ship(1, /*request_id=*/1, /*session_id=*/99, /*sequence=*/1, 4000, Role::kReadWrite,
                              "INSERT INTO t VALUES ('a', 1)")
                    .ok());
    // A forged reply on the right request id but the wrong identity.
    auto forged = ShippedStatementReplyOf(/*session_id=*/99, /*sequence=*/2, Status::OK(),
                                          "INSERTED oid=4000 id=1 page=1 slot=1");
    ASSERT_TRUE(forged.ok());
    sched::SubmitSendPod(*owner_, *transport_, /*src=*/1, /*dst=*/0, /*session_core=*/0,
                         /*request_id=*/1, sched::RingMessageKind::kShippedStatementReply,
                         forged.value());
    Pump();

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(client_->identity_mismatches(), 1u);
    // Both halves, because a bare "the forged text is not there" would also
    // pass if *nothing* arrived: the genuine reply must have landed, and
    // the forged one must not have been what landed.
    ASSERT_TRUE(out->arrived);
    EXPECT_TRUE(out->status.ok()) << out->status.message();
    EXPECT_EQ(out->text, "OK");
}

TEST_F(StatementShipTest, ALateReplyIsCountedRatherThanDelivered) {
    // Once the deadline has fired the statement has been answered. A reply
    // arriving afterwards cannot be un-answered and the owner's effect
    // cannot be undone - so it is counted, and that count is the measure of
    // how often SS3's dedup record is what saves a client.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());
    clock_.Advance(kShippedStatementDeadlineNs);
    ASSERT_TRUE(client_->Settled(1));
    client_->Close(1);  // what the parked statement does when it gives up

    Pump();
    EXPECT_EQ(client_->late_executed_replies(), 1u);
    EXPECT_EQ(client_->late_refused_replies(), 0u);
    EXPECT_EQ(client_->waiting(), 0u);
}

TEST_F(StatementShipTest, AnOverLongStatementIsRefusedAndShipsNothing) {
    std::vector<std::string> seen;
    InstallOwner([&](StatementShipServer::ShippedStatement st,
                     StatementShipServer::ReplyFn reply) {
        seen.emplace_back(std::move(st.text));
        reply(Status::OK(), "OK");
    });

    const std::string too_long(kShippedStatementTextMax + 1, 'x');
    Status refused = client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, too_long);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kUnsupported);
    EXPECT_NE(refused.message().find(std::to_string(kShippedStatementTextMax)),
              std::string::npos)
        << refused.message();
    // Nothing was sent, so no waiter was opened: a waiter here would cost
    // the statement a deadline to learn what is already known.
    EXPECT_EQ(client_->waiting(), 0u);

    // And the longest statement that *does* fit ships **and arrives whole**:
    // the boundary is only pinned if the far side sees every byte.
    const std::string exact(kShippedStatementTextMax, 'y');
    EXPECT_TRUE(client_->Ship(1, 2, 99, 2, 4000, Role::kReadWrite, exact).ok());
    EXPECT_EQ(client_->waiting(), 1u);
    Pump();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].size(), kShippedStatementTextMax);
    EXPECT_EQ(seen[0], exact);
}

TEST_F(StatementShipTest, AnOverLongReplyIsUnknownOutcomeBecauseTheStatementAlreadyRan) {
    // The statement executed; only its answer will not fit. Truncating it
    // would hand the client a different answer, so the honest report is
    // that the effect stands and the answer is lost - which is the same
    // class as a deadline and wears the same code.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) {
        reply(Status::OK(), std::string(kShippedStatementReplyTextMax + 1, 'r'));
    });

    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadOnly, "SELECT * FROM t").ok());
    Pump();

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived) << "it must answer, not leave the caller to the deadline";
    EXPECT_EQ(out->status.code(), StatusCode::kUnknownOutcome);
    EXPECT_FALSE(out->status.retryable());
    EXPECT_NE(out->status.message().find("effect stands"), std::string::npos)
        << out->status.message();
}

TEST_F(StatementShipTest, TheUnknownOutcomeSpellingIsWhatTheClientSees) {
    // The token is a compatibility surface and its `retryable=0` is D4's
    // whole guarantee reaching the wire. Pinned here for the reason
    // ASSERTION_VIOLATION's spelling was pinned before its producer
    // existed: a client written against it must not see it change.
    EXPECT_EQ(ErrorReply(Status::UnknownOutcome("the outcome cannot be stated")),
              "ERR UNKNOWN_OUTCOME retryable=0 the outcome cannot be stated");
}

TEST_F(StatementShipTest, EveryErrorReplyRoundTripsThroughItsCode) {
    // The property SS3's answer path stands on: a statement refused on its
    // owner is rendered, its code is recovered, it crosses, and the arrival
    // core renders it again - and a client must not be able to tell that
    // happened. Every spelling `ErrorReply` has, including the bare arm,
    // because a code that falls through to bare must come back rendering
    // bare.
    const Status statuses[] = {
        Status::TxnConflict("row id=42 was written by transaction 118"),
        Status::FkViolation("parent row 7 does not exist"),
        Status::AssertionViolation("group total exceeds its bound"),
        Status::UnknownOutcome("the statement's effect stands and its answer is lost"),
        Status::InvalidArgument("table 'nosuch' not found"),
        Status::Unsupported("outer joins are not executed"),
        Status::NotFound("index 'ix' not found"),
        Status::IoError("the device refused the write"),
    };
    for (const Status& status : statuses) {
        const std::string rendered = ErrorReply(status);
        const Status recovered = StatusFromErrorReply(rendered);
        EXPECT_EQ(ErrorReply(recovered), rendered) << rendered;
        // And the bit itself, which is the half a client acts on.
        EXPECT_EQ(recovered.retryable(), status.retryable()) << rendered;
    }

    // A line that is not a refusal is not turned into one: the caller has
    // already decided which arm it is on.
    EXPECT_TRUE(StatusFromErrorReply("INSERTED oid=4000 id=7 page=12 slot=3").ok());
}

TEST_F(StatementShipTest, TheArrivalCoresRankCrossesWithTheStatement) {
    // SS3 runs the statement under the rank the arrival core authenticated,
    // so the rank has to arrive with it - a `Session` the owner mints holds
    // kAdmin by default, and an owner that assumed one would be the only
    // authorization there is.
    Role seen = Role::kAdmin;
    InstallOwner([&](StatementShipServer::ShippedStatement st,
                     StatementShipServer::ReplyFn reply) {
        seen = st.role;
        reply(Status::OK(), "OK");
    });
    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadOnly, "SELECT * FROM t").ok());
    Pump();
    EXPECT_EQ(seen, Role::kReadOnly);
}

TEST_F(StatementShipTest, ARoleByteThisBuildCannotReadIsRefusedNotAssumed) {
    // Fail closed. A byte outside the enum means the two ends disagree
    // about what a rank is, and the wrong reading of that is to pick one -
    // in either direction: `kAdmin` runs a statement nobody authorized,
    // `kReadOnly` refuses one that was.
    bool executed = false;
    InstallOwner([&](StatementShipServer::ShippedStatement,
                     StatementShipServer::ReplyFn reply) {
        executed = true;
        reply(Status::OK(), "OK");
    });

    auto forged = ShippedStatementRequestOf(99, 1, 4000, Role::kReadWrite, "SELECT * FROM t");
    ASSERT_TRUE(forged.ok()) << forged.status().message();
    ASSERT_TRUE(ShippedStatementRoleOf(forged.value()).ok());  // as issued
    forged.value().role = 99;  // no such rank
    EXPECT_FALSE(ShippedStatementRoleOf(forged.value()).ok());

    // And the seam refuses it rather than running it. It **replies**: a
    // refusal the arrival core can read costs it one round trip, where
    // silence would cost it a whole deadline and answer `UnknownOutcome`
    // for a statement that provably never ran.
    sched::SubmitSendPod(*arrival_, *transport_, 0, 1, /*session_core=*/0, /*request_id=*/1,
                         sched::RingMessageKind::kShippedStatementRequest, forged.value());
    Pump();
    EXPECT_FALSE(executed) << "a rank this build cannot read must not run a statement";
    EXPECT_EQ(server_->replies(), 1u);
}

TEST_F(StatementShipTest, AFullRingRetriesAndNeverDrops) {
    // The fixture's ring holds 16 slots. Shipping past that before a single
    // pump forces `MakeSendRetryTask`'s ResourceExhausted path, which is
    // the one property it gives and nothing else here asserts: a full ring
    // must cost retries, never a lost statement - a lost one costs its
    // client a 10 s park and a false `UnknownOutcome`.
    int executed = 0;
    InstallOwner([&](StatementShipServer::ShippedStatement,
                     StatementShipServer::ReplyFn reply) {
        ++executed;
        reply(Status::OK(), "OK");
    });

    constexpr int kShipped = 40;  // well past the 16 slots
    for (int i = 0; i < kShipped; ++i) {
        ASSERT_TRUE(client_
                        ->Ship(1, /*request_id=*/static_cast<std::uint64_t>(i + 1),
                               /*session_id=*/99, /*sequence=*/static_cast<std::uint64_t>(i),
                               4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)")
                        .ok())
            << i;
    }
    EXPECT_EQ(client_->waiting(), static_cast<std::size_t>(kShipped));

    Pump(400);
    EXPECT_EQ(executed, kShipped) << "a full ring dropped a statement instead of retrying";
    for (int i = 0; i < kShipped; ++i) {
        const ShippedStatementOutcome* out =
            client_->Find(static_cast<std::uint64_t>(i + 1));
        ASSERT_NE(out, nullptr) << i;
        EXPECT_TRUE(out->arrived) << i;
        EXPECT_TRUE(out->status.ok()) << i << ": " << out->status.message();
    }
    EXPECT_EQ(client_->identity_mismatches(), 0u);
}

TEST_F(StatementShipTest, ARequestIdWithAStatementParkedOnItIsRefused) {
    // Reusing a live id would replace the parked statement's waiter,
    // identity and all - after which the reply check has nothing left to
    // compare against and the first statement would be woken with the
    // second's answer.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());
    Status refused = client_->Ship(1, 1, 99, 2, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('b', 2)");
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(client_->waiting(), 1u);

    // And the id is reusable once the statement that held it has closed.
    Pump();
    client_->Close(1);
    EXPECT_TRUE(client_->Ship(1, 1, 99, 2, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('b', 2)").ok());
}

TEST_F(StatementShipTest, AShipToACoreTheInstanceHasNotIsRefusedBeforeAnythingParks) {
    // The one send failure `MakeSendRetryTask` does not retry. Left to it,
    // the message evaporates and the statement parks the full deadline
    // before being told its outcome is unknown - for a request that
    // provably never left this core.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    Status refused = client_->Ship(/*owner_core=*/7, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES (1)");
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(client_->waiting(), 0u);
}

TEST_F(StatementShipTest, AForgedReplyLengthIsUnknownOutcomeNotABlankAnswer) {
    // A reply whose `text_len` is not a length a reply can hold cannot be
    // trusted for anything, `status_code` included - so clamping the length
    // and reading the code would hand the client an *empty line as the
    // statement's answer*. The honest reading is the deadline's, reached
    // early.
    // No owner handler at all, so the request is dropped on arrival and the
    // forged reply is the only thing the waiter ever sees.
    ASSERT_TRUE(client_->Ship(1, 1, 99, 1, 4000, Role::kReadWrite, "INSERT INTO t VALUES ('a', 1)").ok());

    ShippedStatementReplyPayload forged{};
    forged.session_id = 99;
    forged.sequence = 1;
    forged.status_code = static_cast<std::uint32_t>(StatusCode::kOk);
    forged.text_len = static_cast<std::uint16_t>(kShippedStatementReplyTextMax + 1);
    sched::SubmitSendPod(*owner_, *transport_, 1, 0, /*session_core=*/0, /*request_id=*/1,
                         sched::RingMessageKind::kShippedStatementReply, forged);
    Pump();

    const ShippedStatementOutcome* out = client_->Find(1);
    ASSERT_NE(out, nullptr);
    ASSERT_TRUE(out->arrived);
    EXPECT_EQ(out->status.code(), StatusCode::kUnknownOutcome);
    EXPECT_FALSE(out->status.retryable());
    EXPECT_TRUE(out->text.empty());
}

TEST_F(StatementShipTest, AWrongSizedRequestGetsNoReplyAndLeavesTheDeadlineToAnswer) {
    // The one path that cannot answer: nothing in a payload of the wrong
    // size names the waiter to answer to. It must not crash, must not
    // reply, and must leave the arrival core's deadline as the backstop.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    const std::byte runt[8]{};
    sched::MessageHeader header{};
    header.request_id = 1;
    header.dst_core = 1;
    header.kind = static_cast<std::uint16_t>(sched::RingMessageKind::kShippedStatementRequest);
    header.sched_group = static_cast<std::uint16_t>(sched::SchedulingGroup::kSystem);
    arrival_->Submit(sched::MakeSendRetryTask(*transport_, header,
                                              std::span<const std::byte>(runt, sizeof(runt))));
    Pump();
    EXPECT_EQ(server_->requests(), 1u);
    EXPECT_EQ(server_->replies(), 0u);
}

TEST_F(StatementShipTest, AForgedTextLengthIsRefusedNotRead) {
    // The payload is bytes this core did not compute, and `text_len` is
    // the only thing between a forged length and a read past the array.
    InstallOwner([](StatementShipServer::ShippedStatement,
                    StatementShipServer::ReplyFn reply) { reply(Status::OK(), "OK"); });

    ShippedStatementRequestPayload forged{};
    forged.session_id = 99;
    forged.sequence = 1;
    forged.text_len = static_cast<std::uint16_t>(kShippedStatementTextMax + 1);
    EXPECT_FALSE(ShippedStatementTextOf(forged).ok());
    forged.text_len = 0;
    EXPECT_FALSE(ShippedStatementTextOf(forged).ok());
}

}  // namespace
}  // namespace kds::server
