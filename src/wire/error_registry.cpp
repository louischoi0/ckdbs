#include "kds/wire/error_registry.hpp"

#include <utility>

#include "kds/wire/kwp_types.hpp"

// The Status -> wire mapping (docs/spec/protocol.md §11, protocol-wp.md
// P12). The rules this file obeys are stated on the header; what is here is
// the table and the codec.

namespace kds::wire {

ErrorCategory CategoryOf(StatusCode code) noexcept {
    switch (code) {
        // Every engine code that has a category of the same name maps to
        // it. Written as a switch rather than an array indexed by
        // StatusCode so a code appended to status.hpp is a **compile
        // warning** here (-Wswitch, which this tree builds with) rather
        // than a silent kInternal on the wire - the failure that mapping
        // table would produce is a new engine condition arriving at every
        // client as "internal error", which no client can act on.
        case StatusCode::kInvalidArgument: return ErrorCategory::kInvalidArgument;
        case StatusCode::kNotFound: return ErrorCategory::kNotFound;
        case StatusCode::kAlreadyExists: return ErrorCategory::kAlreadyExists;
        case StatusCode::kOutOfSpace: return ErrorCategory::kOutOfSpace;
        case StatusCode::kOutOfRange: return ErrorCategory::kOutOfRange;
        case StatusCode::kCorruption: return ErrorCategory::kCorruption;
        case StatusCode::kIoError: return ErrorCategory::kIoError;
        case StatusCode::kUnsupported: return ErrorCategory::kUnsupported;
        // The other half of the refusal pair (status.hpp, 2026-08-31): what
        // the design admits and nobody built, against what the architecture
        // cannot admit. Its own category because the distinction exists for
        // a client library's feature detection, and one category would
        // answer neither question.
        case StatusCode::kNotImplemented: return ErrorCategory::kNotImplemented;
        case StatusCode::kTxnConflict: return ErrorCategory::kTxnConflict;
        case StatusCode::kCardinalityViolation:
            return ErrorCategory::kCardinalityViolation;
        case StatusCode::kResourceExhausted: return ErrorCategory::kResourceExhausted;
        case StatusCode::kUnknownOutcome: return ErrorCategory::kUnknownOutcome;
        case StatusCode::kFkViolation: return ErrorCategory::kFkViolation;
        case StatusCode::kAssertionViolation: return ErrorCategory::kAssertionViolation;
        // Not an error, and reached only by a caller that ignored the
        // header's contract. kInternal rather than a crash: a server that
        // dies because it was asked to frame a success is worse than one
        // that reports a defect it can name. `ErrorFromStatus` refuses it
        // before this is consulted.
        case StatusCode::kOk: return ErrorCategory::kInternal;
    }
    return ErrorCategory::kInternal;
}

std::string_view ErrorCategoryName(ErrorCategory category) noexcept {
    switch (category) {
        case ErrorCategory::kInvalidArgument: return "INVALID_ARGUMENT";
        case ErrorCategory::kNotFound: return "NOT_FOUND";
        case ErrorCategory::kAlreadyExists: return "ALREADY_EXISTS";
        case ErrorCategory::kOutOfSpace: return "OUT_OF_SPACE";
        case ErrorCategory::kOutOfRange: return "OUT_OF_RANGE";
        case ErrorCategory::kCorruption: return "CORRUPTION";
        case ErrorCategory::kIoError: return "IO_ERROR";
        case ErrorCategory::kInternal: return "INTERNAL";
        case ErrorCategory::kProtocol: return "PROTOCOL";
        case ErrorCategory::kUnsupported: return "UNSUPPORTED";
        case ErrorCategory::kTxnConflict: return "TXN_CONFLICT";
        case ErrorCategory::kCancelled: return "CANCELLED";
        case ErrorCategory::kCardinalityViolation: return "CARDINALITY_VIOLATION";
        case ErrorCategory::kResourceExhausted: return "RESOURCE_EXHAUSTED";
        case ErrorCategory::kUnknownOutcome: return "UNKNOWN_OUTCOME";
        case ErrorCategory::kNotImplemented: return "NOT_IMPLEMENTED";
        case ErrorCategory::kFkViolation: return "FK_VIOLATION";
        case ErrorCategory::kAssertionViolation: return "ASSERTION_VIOLATION";
    }
    return "INTERNAL";
}

WireError ErrorFromStatus(const Status& status, std::uint16_t detail) {
    WireError out;
    // `kOk` has no engine spelling for "internal" to fall back on -
    // StatusCode has no such code, because the engine never needs one - so
    // the category is named directly. `CategoryOf(kOk)` answers the same
    // thing; naming it here keeps the two arms of this function reading
    // alike.
    out.code = status.ok() ? MakeErrorCode(ErrorCategory::kInternal, detail)
                           : MakeErrorCode(CategoryOf(status.code()), detail);
    // R1: asked, never asserted. status.hpp's `retryable()` is
    // `IsRetryable(code_)`, so this is the engine's own answer travelling
    // outward unchanged.
    out.retryable = !status.ok() && IsRetryable(status.code());
    // An engine failure never closes the connection (§11); only framing
    // corruption and handshake failures do, and neither of those has a
    // Status.
    out.severity = Severity::kError;
    out.message = status.ok() ? "internal: a success was framed as an error"
                              : status.message();
    return out;
}

WireError ProtocolError(ProtocolDetail detail, std::string message, Severity severity) {
    WireError out;
    out.code = MakeErrorCode(ErrorCategory::kProtocol, static_cast<std::uint16_t>(detail));
    // Never. A client that sent a frame this server could not read will
    // send it again on a retry, and a retry loop on a protocol defect is
    // an infinite one.
    out.retryable = false;
    out.severity = severity;
    out.message = std::move(message);
    return out;
}

std::vector<std::byte> EncodeError(const WireError& error) {
    PayloadWriter w;
    w.U32(error.code);
    w.U8(error.retryable ? 1 : 0);
    w.U8(static_cast<std::uint8_t>(error.severity));
    w.Text(error.message);
    if (error.detail.has_value()) {
        w.Text(*error.detail);
    } else {
        w.AbsentText();
    }
    // The same absent convention one field over, on a fixed-width field:
    // 0xFFFFFFFF rather than 0, because byte 0 is where a statement's very
    // first token sits and "the error is at byte 0" must not decode as
    // "there is no position".
    w.U32(error.position.value_or(0xFFFFFFFFu));
    return w.Take();
}

StatusOr<WireError> DecodeError(std::span<const std::byte> payload) {
    PayloadReader r(payload);
    WireError out;
    auto code = r.U32();
    auto retryable = r.U8();
    auto severity = r.U8();
    auto message = r.Text();
    auto detail = r.Text();
    auto position = r.U32();
    if (!code.has_value() || !retryable.has_value() || !severity.has_value() ||
        !message.has_value() || !message->has_value() || !detail.has_value() ||
        !position.has_value()) {
        return Status::Corruption("S_ERROR payload is truncated");
    }
    out.code = *code;
    out.retryable = *retryable != 0;
    // Anything other than the two defined values decodes as `kFatal`, the
    // safe reading: a client that cannot tell whether the connection
    // survives must assume it did not, and reconnecting costs a round trip
    // where guessing the other way costs a hang.
    out.severity = *severity == static_cast<std::uint8_t>(Severity::kError) ? Severity::kError
                                                                           : Severity::kFatal;
    out.message = std::move(**message);
    out.detail = std::move(*detail);
    if (*position != 0xFFFFFFFFu) out.position = *position;
    return out;
}

}  // namespace kds::wire
