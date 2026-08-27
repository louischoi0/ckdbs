#include "kds/server/txn_2pc_service.hpp"

#include <string>

namespace kds::server {

StatusOr<TxnDecision> TxnDecisionOf(const TxnDecideRequestPayload& decide) {
    switch (static_cast<TxnDecision>(decide.decision)) {
        case TxnDecision::kCommit: return TxnDecision::kCommit;
        case TxnDecision::kAbort: return TxnDecision::kAbort;
        case TxnDecision::kUnset: break;
    }
    // Neither defaulted nor guessed. `kUnset` is the zeroed buffer and any
    // other byte is two ends disagreeing about what a decision is; reading
    // either as commit would apply a transaction nobody decided, and as
    // abort would discard one that may already be committed in the
    // coordinator's stream - which is the decision, by D4.
    return Status::InvalidArgument("cross-owner transaction: decide names decision " +
                                   std::to_string(decide.decision) +
                                   ", which is not a decision this build knows; the "
                                   "participant stays in doubt rather than guessing");
}

StatusOr<std::string_view> TxnParticipantReplyMessageOf(const TxnParticipantReplyPayload& reply) {
    if (reply.message_len > kTxnParticipantReplyMessageMax) {
        return Status::InvalidArgument("cross-owner transaction: participant reply names a "
                                       "message of " +
                                       std::to_string(reply.message_len) +
                                       " bytes, which is not a length this payload can hold");
    }
    return std::string_view(reply.message, reply.message_len);
}

}  // namespace kds::server
