#include "kds/server/txn_2pc_service.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/send_retry.hpp"

// R6-1: the cross-owner transaction wire, and D6's sizing answer.
//
// Nothing sends these yet - the waiter, the durable prepare and the decision
// record are R6-3's - so what is pinned here is what R6-1 actually produces,
// in the order it would hurt:
//
//   1. **D6's answer as a number.** The order requires R6-1 to confirm the
//      messages fit the ring slot rather than assume it, and to stop if any
//      does not. A test that names the sizes is what makes a later field
//      addition announce itself instead of silently spending the headroom;
//   2. the four kinds are **known and named** - a kind added to the enum but
//      missing from `IsKnownRingMessageKind` is dropped by the receiver, and
//      from the sender's side that is indistinguishable from a slow peer;
//   3. the payloads survive a real ring byte for byte, since a POD that does
//      not is the one failure a wire header cannot talk its way out of;
//   4. the decodes **refuse rather than guess** - a zeroed decision byte and
//      a forged message length are both bytes this core did not compute.

namespace kds::server {
namespace {

TEST(Txn2pcWireTest, EveryR6MessageFitsOneRingSlotWithRoomToSpare) {
    // **D6, answered rather than assumed.** The work order says that if any
    // R6 message needs more than the slot, `crosscore.md` §9's payload
    // sizing decision becomes R6's gate and the order stops until it is
    // taken - and that no field may be shrunk to avoid that. It does not
    // come to that: the two request legs are 24 bytes and the reply is 128,
    // against a 1,024-byte slot.
    EXPECT_EQ(sizeof(TxnPrepareRequestPayload), 24u);
    EXPECT_EQ(sizeof(TxnDecideRequestPayload), 24u);
    EXPECT_EQ(sizeof(TxnParticipantReplyPayload), 256u);
    EXPECT_EQ(sched::kCoreRingPayloadBytes, 1024u);

    // The margin, stated as a relation and not as a second literal: the
    // largest R6 message is a quarter of the slot, so the sizing decision is
    // this order's neighbour and not its gate.
    EXPECT_LE(sizeof(TxnParticipantReplyPayload), sched::kCoreRingPayloadBytes / 4);
}

TEST(Txn2pcWireTest, TheReplyHoldsTheRefusalsAParticipantCanActuallyProduce) {
    // **The cap is derived from this list, so the list is the test.** The
    // first cut of this file chose 104 bytes to land `sizeof` on 128, which
    // was three bytes under the engine's most likely prepare failure - and
    // the first version of the round-trip test below quietly used a
    // shortened copy of that very message, which is how a cap sized to a
    // round number hides. These are the real strings.
    const std::string spent_lease =
        "extent lease: this core's lease of 64 pages is spent; a refill must be granted "
        "before it can allocate again";
    EXPECT_EQ(spent_lease.size(), 107u) << "the refusal this cap was measured against moved";
    EXPECT_LE(spent_lease.size(), kTxnParticipantReplyMessageMax);

    // The longest of the measured population, and the one that sets the
    // margin. If a future refusal outgrows this, the cap moves - it is not
    // the message that gets cut.
    EXPECT_GE(kTxnParticipantReplyMessageMax, 157u);

    // Derived, not chosen: the fixed part plus the message is the whole
    // payload, so a field added to the header cannot silently eat the text.
    EXPECT_EQ(kTxnParticipantReplyFixedBytes + kTxnParticipantReplyMessageMax,
              kTxnParticipantReplyBytes);
}

TEST(Txn2pcWireTest, TheFourKindsAreKnownAndNamedAndCollideWithNothing) {
    // A kind the enum carries but `IsKnownRingMessageKind` does not is
    // **dropped on arrival**, and the coordinator would pay a full deadline
    // to learn that its build disagrees with itself.
    const sched::RingMessageKind kinds[] = {
        sched::RingMessageKind::kTxnPrepareRequest,
        sched::RingMessageKind::kTxnPrepareReply,
        sched::RingMessageKind::kTxnDecideRequest,
        sched::RingMessageKind::kTxnDecideReply,
    };
    for (const sched::RingMessageKind kind : kinds) {
        EXPECT_TRUE(sched::IsKnownRingMessageKind(static_cast<std::uint16_t>(kind)))
            << sched::RingMessageKindName(kind);
        EXPECT_STRNE(sched::RingMessageKindName(kind), "unknown");
    }

    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kTxnPrepareRequest),
                 "TXN_PREPARE_REQUEST");
    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kTxnPrepareReply),
                 "TXN_PREPARE_REPLY");
    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kTxnDecideRequest),
                 "TXN_DECIDE_REQUEST");
    EXPECT_STREQ(sched::RingMessageKindName(sched::RingMessageKind::kTxnDecideReply),
                 "TXN_DECIDE_REPLY");

    // The values, pinned because central enumeration is the whole point of
    // that header: a second subsystem reusing 33-36 would be two protocols
    // on one number, which the receiver cannot detect.
    EXPECT_EQ(static_cast<std::uint16_t>(sched::RingMessageKind::kTxnPrepareRequest), 33u);
    EXPECT_EQ(static_cast<std::uint16_t>(sched::RingMessageKind::kTxnPrepareReply), 34u);
    EXPECT_EQ(static_cast<std::uint16_t>(sched::RingMessageKind::kTxnDecideRequest), 35u);
    EXPECT_EQ(static_cast<std::uint16_t>(sched::RingMessageKind::kTxnDecideReply), 36u);
}

TEST(Txn2pcWireTest, AZeroedDecideDecodesAsNoDecisionAtAll) {
    // The zero-collision rule `RingMessageKind` and `StoredAccessKind` each
    // keep. Of the two real values, the one a zeroed buffer would otherwise
    // decode as is `kCommit` - applying a transaction nobody decided.
    TxnDecideRequestPayload zeroed{};
    EXPECT_FALSE(TxnDecisionOf(zeroed).ok());
    EXPECT_EQ(static_cast<std::uint8_t>(TxnDecision::kUnset), 0u);
}

TEST(Txn2pcWireTest, ADecisionByteThisBuildCannotReadLeavesTheParticipantInDoubt) {
    // Fail-closed, and neither value is "closed" here: a prepared
    // participant may not unilaterally abort and an unprepared one may not
    // commit, so the only honest answer to an unreadable byte is to refuse
    // and stay in doubt - which is the state D5's resolution ask ends.
    TxnDecideRequestPayload decide{};
    decide.decision = 99;
    const auto refused = TxnDecisionOf(decide);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(refused.status().message().find("in doubt"), std::string::npos)
        << refused.status().message();

    // And the two it can read, on the same field.
    decide.decision = static_cast<std::uint8_t>(TxnDecision::kCommit);
    ASSERT_TRUE(TxnDecisionOf(decide).ok());
    EXPECT_EQ(TxnDecisionOf(decide).value(), TxnDecision::kCommit);
    decide.decision = static_cast<std::uint8_t>(TxnDecision::kAbort);
    ASSERT_TRUE(TxnDecisionOf(decide).ok());
    EXPECT_EQ(TxnDecisionOf(decide).value(), TxnDecision::kAbort);
}

TEST(Txn2pcWireTest, AForgedReplyMessageLengthIsRefusedNotRead) {
    // `message_len` is the only thing between a forged length and a read
    // past the array - the reason this payload carries a length at all
    // rather than a NUL that may simply be absent.
    TxnParticipantReplyPayload reply{};
    reply.message_len = static_cast<std::uint16_t>(kTxnParticipantReplyMessageMax + 1);
    EXPECT_FALSE(TxnParticipantReplyMessageOf(reply).ok());

    // An empty message is legal, because a success carries none.
    reply.message_len = 0;
    auto empty = TxnParticipantReplyMessageOf(reply);
    ASSERT_TRUE(empty.ok()) << empty.status().message();
    EXPECT_TRUE(empty.value().empty());

    // And the longest that fits arrives whole: the boundary is only pinned
    // if the far side sees every byte of it.
    const std::string full(kTxnParticipantReplyMessageMax, 'r');
    reply.message_len = static_cast<std::uint16_t>(full.size());
    std::memcpy(reply.message, full.data(), full.size());
    auto at_cap = TxnParticipantReplyMessageOf(reply);
    ASSERT_TRUE(at_cap.ok()) << at_cap.status().message();
    EXPECT_EQ(at_cap.value(), full);
}

// ---- The payloads over a real ring -----------------------------------------

class Txn2pcRingTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/2, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        coordinator_.emplace(clock_, io0_);
        participant_.emplace(clock_, io1_);
        ASSERT_TRUE(coordinator_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(participant_->AttachTransport(&*transport_, 1).ok());
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            coordinator_->RunOnce();
            participant_->RunOnce();
        }
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> coordinator_;
    std::optional<sched::Scheduler> participant_;
};

TEST_F(Txn2pcRingTest, APrepareCrossesByteForByte) {
    std::vector<TxnPrepareRequestPayload> seen;
    ASSERT_TRUE(participant_
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kTxnPrepareRequest,
                        [&](const sched::MessageHeader& header,
                            std::span<const std::byte> payload) {
                            // The coordinator's core is the identity's third
                            // component and is read from the header, never
                            // repeated in the payload (the header's argument).
                            EXPECT_EQ(header.src_core, 0u);
                            ASSERT_EQ(payload.size(), sizeof(TxnPrepareRequestPayload));
                            TxnPrepareRequestPayload got{};
                            std::memcpy(&got, payload.data(), sizeof(got));
                            seen.push_back(got);
                        })
                    .ok());

    TxnPrepareRequestPayload prepare{};
    prepare.session_id = 99;
    prepare.transaction_id = 4242;
    prepare.retry = 1;
    sched::SubmitSendPod(*coordinator_, *transport_, /*src=*/0, /*dst=*/1, /*session_core=*/0,
                         /*request_id=*/7, sched::RingMessageKind::kTxnPrepareRequest, prepare);
    Pump();

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].session_id, 99u);
    EXPECT_EQ(seen[0].transaction_id, 4242u);
    EXPECT_EQ(seen[0].retry, 1u);
}

TEST_F(Txn2pcRingTest, ADecideCrossesAndItsDecisionDecodesOnTheFarSide) {
    std::vector<TxnDecision> seen;
    ASSERT_TRUE(participant_
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kTxnDecideRequest,
                        [&](const sched::MessageHeader&, std::span<const std::byte> payload) {
                            ASSERT_EQ(payload.size(), sizeof(TxnDecideRequestPayload));
                            TxnDecideRequestPayload got{};
                            std::memcpy(&got, payload.data(), sizeof(got));
                            auto decision = TxnDecisionOf(got);
                            ASSERT_TRUE(decision.ok()) << decision.status().message();
                            seen.push_back(decision.value());
                        })
                    .ok());

    TxnDecideRequestPayload commit{};
    commit.session_id = 99;
    commit.transaction_id = 4242;
    commit.decision = static_cast<std::uint8_t>(TxnDecision::kCommit);
    sched::SubmitSendPod(*coordinator_, *transport_, 0, 1, 0, /*request_id=*/8,
                         sched::RingMessageKind::kTxnDecideRequest, commit);

    TxnDecideRequestPayload abort{};
    abort.session_id = 99;
    abort.transaction_id = 4243;
    abort.decision = static_cast<std::uint8_t>(TxnDecision::kAbort);
    sched::SubmitSendPod(*coordinator_, *transport_, 0, 1, 0, /*request_id=*/9,
                         sched::RingMessageKind::kTxnDecideRequest, abort);
    Pump();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], TxnDecision::kCommit);
    EXPECT_EQ(seen[1], TxnDecision::kAbort);
}

TEST_F(Txn2pcRingTest, AParticipantsRefusalCrossesAsItsCodeAndItsWords) {
    // The reply legs carry a StatusCode and a message so `Status::FromWire`
    // rebuilds the participant's own spelling, `retryable` bit included -
    // the same property SS1 pins for a shipped statement, and the reason
    // this payload has a message field at all.
    std::optional<Status> seen;
    ASSERT_TRUE(coordinator_
                    ->RegisterMessageHandler(
                        sched::RingMessageKind::kTxnPrepareReply,
                        [&](const sched::MessageHeader&, std::span<const std::byte> payload) {
                            ASSERT_EQ(payload.size(), sizeof(TxnParticipantReplyPayload));
                            TxnParticipantReplyPayload got{};
                            std::memcpy(&got, payload.data(), sizeof(got));
                            EXPECT_EQ(got.session_id, 99u);
                            EXPECT_EQ(got.transaction_id, 4242u);
                            auto message = TxnParticipantReplyMessageOf(got);
                            ASSERT_TRUE(message.ok()) << message.status().message();
                            seen = Status::FromWire(got.status_code, std::string(message.value()));
                        })
                    .ok());

    // The **whole** refusal, second clause included. An earlier version of
    // this test cut it to fit a 104-byte cap, which is exactly the failure
    // the cap's derivation now exists to prevent.
    const Status refusal = Status::TxnConflict(
        "extent lease: this core's lease of 64 pages is spent; a refill must be granted "
        "before it can allocate again");
    TxnParticipantReplyPayload reply{};
    reply.session_id = 99;
    reply.transaction_id = 4242;
    reply.status_code = static_cast<std::uint32_t>(refusal.code());
    reply.message_len = static_cast<std::uint16_t>(refusal.message().size());
    ASSERT_LE(refusal.message().size(), kTxnParticipantReplyMessageMax);
    std::memcpy(reply.message, refusal.message().data(), refusal.message().size());
    sched::SubmitSendPod(*participant_, *transport_, /*src=*/1, /*dst=*/0, /*session_core=*/0,
                         /*request_id=*/7, sched::RingMessageKind::kTxnPrepareReply, reply);
    Pump();

    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(seen->retryable());
    // The whole message, not a prefix of it: a cap that truncated here would
    // drop the half that tells the operator what to do about it.
    EXPECT_EQ(seen->message(), refusal.message());
}

TEST_F(Txn2pcRingTest, OneReplyPayloadServesBothReplyLegs) {
    // **R6-1's own decision, pinned.** A prepare reply and a decide ack
    // differ in the message kind and in no field, so they share a payload -
    // and the way that decision fails is one leg growing a field the other
    // does not have. Both legs are sent here, on the same struct, and both
    // arrive readable.
    //
    // It also pins the success arm: `status_code` 0 is "prepared" (or
    // "applied"), and carries no message - legal by construction, since a
    // participant with nothing to refuse has nothing to say.
    std::vector<std::pair<sched::RingMessageKind, Status>> seen;
    auto receive = [&](sched::RingMessageKind kind) {
        return [&, kind](const sched::MessageHeader&, std::span<const std::byte> payload) {
            ASSERT_EQ(payload.size(), sizeof(TxnParticipantReplyPayload));
            TxnParticipantReplyPayload got{};
            std::memcpy(&got, payload.data(), sizeof(got));
            EXPECT_EQ(got.session_id, 99u);
            EXPECT_EQ(got.transaction_id, 4242u);
            auto message = TxnParticipantReplyMessageOf(got);
            ASSERT_TRUE(message.ok()) << message.status().message();
            EXPECT_TRUE(message.value().empty());
            seen.emplace_back(kind, Status::FromWire(got.status_code,
                                                     std::string(message.value())));
        };
    };
    ASSERT_TRUE(coordinator_
                    ->RegisterMessageHandler(sched::RingMessageKind::kTxnPrepareReply,
                                             receive(sched::RingMessageKind::kTxnPrepareReply))
                    .ok());
    ASSERT_TRUE(coordinator_
                    ->RegisterMessageHandler(sched::RingMessageKind::kTxnDecideReply,
                                             receive(sched::RingMessageKind::kTxnDecideReply))
                    .ok());

    TxnParticipantReplyPayload ok{};
    ok.session_id = 99;
    ok.transaction_id = 4242;
    ok.status_code = static_cast<std::uint32_t>(StatusCode::kOk);
    sched::SubmitSendPod(*participant_, *transport_, 1, 0, 0, /*request_id=*/7,
                         sched::RingMessageKind::kTxnPrepareReply, ok);
    sched::SubmitSendPod(*participant_, *transport_, 1, 0, 0, /*request_id=*/8,
                         sched::RingMessageKind::kTxnDecideReply, ok);
    Pump();

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].first, sched::RingMessageKind::kTxnPrepareReply);
    EXPECT_TRUE(seen[0].second.ok()) << seen[0].second.message();
    EXPECT_EQ(seen[1].first, sched::RingMessageKind::kTxnDecideReply);
    EXPECT_TRUE(seen[1].second.ok()) << seen[1].second.message();
}

}  // namespace
}  // namespace kds::server
