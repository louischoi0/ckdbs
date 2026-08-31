#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/wire/kwp.hpp"

// The `S_ERROR` payload and the table that produces it
// (`docs/spec/protocol.md` §11, `docs/inflight/in-progress/protocol-wp.md`
// P12).
//
// ---- What this file is for ----------------------------------------------
//
// A `Status` is the engine's word for "this went wrong"; an `S_ERROR` frame
// is the client's. The translation between them is a **compatibility
// surface**, not a diagnostic: §11 makes `retryable` "authoritative client
// guidance", and a financial client library's retry loop is written against
// it. So the mapping lives in exactly one table, is append-only, and is
// pinned by a golden list that fails on a renumber.
//
// ---- Three rules, and what each one refuses ------------------------------
//
// R1 - **`retryable` is never written by hand.** It comes from
//      `kds::IsRetryable`, the engine's own spelling of the same fact
//      (status.hpp says so, and names this file). A table that carried the
//      bit per entry would let the two drift, and the drift would be
//      silent: a client would retry something the engine knows will fail
//      the same way, or - far worse - not retry something it should. The
//      wire-only categories, which have no `Status`, carry the bit
//      explicitly *because* they have no engine answer to defer to.
//
// R2 - **A category is never invented to be tidy.** `ErrorCategory` mirrors
//      `StatusCode` (kwp.hpp), and a new engine code earns a category
//      rather than being folded into `kInvalidArgument`: a client cannot
//      fix a cardinality violation by fixing its arguments. Folding is what
//      makes an error message the only usable field, and a message is not a
//      surface.
//
// R3 - **`detail` narrows a category; it never replaces one.** A client
//      that understands only the category must still be able to act
//      correctly, so every detail value is a refinement of an answer that
//      is already right without it. `kNoDetail` is the common case and
//      carries no information by design.

namespace kds::wire {

// ---- Severity (§11) ------------------------------------------------------
//
// Exactly the distinction §11 draws and no more: "errors never close the
// connection except framing-level corruption (§2) and handshake failures".
// So the field answers one question - is this connection still usable -
// and a client that reads nothing else can still decide whether to
// reconnect.
//
// Not a log level. A third value meaning "warning" would be a `S_NOTICE`,
// which is its own frame precisely so a non-fatal remark is not an error
// with a softer adjective.
enum class Severity : std::uint8_t {
    kError = 1,  // the statement failed; the connection stands
    kFatal = 2,  // the connection is closed after this frame
};

// ---- Detail codes (§11: append-only, never renumbered) -------------------
//
// Zero in every category means "the category is the whole answer", which is
// what nearly every engine-originated error carries: a `NotFound` naming a
// relation needs no refinement, and inventing one per message would make
// the message the surface again (R3).
//
// The named values below are all `kProtocol`'s, and that is not a
// coincidence: the protocol layer is the only place where the *client's own
// next action* differs within one category. A client that sent an
// unsupported version must renegotiate; one that sent an unknown frame type
// has a version-skewed build; one that named a portal that does not exist
// has a state-machine bug. All three are `kProtocol`, and none of them is
// fixed the same way.
inline constexpr std::uint16_t kNoDetail = 0;

enum class ProtocolDetail : std::uint16_t {
    // The framing itself: a `length` outside [kMinFrameLength, kMaxFrame],
    // or a payload that ends inside a field. Always fatal - §2 says resync
    // is impossible - and it is the one detail a client may see with no
    // session at all.
    kMalformedFrame = 1,
    // A `type` byte this server does not know. Not fatal: the frame is
    // skipped and the session continues at the next `C_SYNC`, because a
    // client built against a later version sending a frame this one has
    // never heard of is exactly what capability bits exist to survive.
    kUnknownFrameType = 2,
    // `C_HELLO` carried a magic that is not 'KWP1'. Fatal: the peer is not
    // speaking this protocol, and there is nothing to negotiate.
    kBadMagic = 3,
    // The client's [min, max] and the server's do not intersect. Fatal,
    // and §3's named case - the client must reconnect with a version this
    // server admits, which the message states.
    kUnsupportedVersion = 4,
    // A frame that is legal in the protocol but not in this state: a
    // `C_EXECUTE` before the handshake, a second `C_HELLO`, a `C_BIND`
    // naming an unparsed statement. Not fatal; the skip-to-`C_SYNC` rule
    // (§5) is what puts the session back on its feet.
    kUnexpectedFrame = 5,
    // A named statement or portal the session does not hold. Its own value
    // rather than `kNotFound`'s category, because what is missing is a
    // *session handle* and not a database object: a client must re-issue
    // `C_PARSE`, not check its schema.
    kUnknownStatement = 6,
    kUnknownPortal = 7,
    // A payload the codec could read but whose contents contradict the
    // frame: a parameter count that disagrees with the parameters that
    // follow, a `C_DESCRIBE` kind byte that is neither statement nor
    // portal.
    kMalformedPayload = 8,
};

// The session's own limits (§10: "server may cap statement/portal counts
// (`ERROR(LIMIT)` beyond)"). Under `kResourceExhausted` rather than
// `kProtocol`, because the client did nothing wrong - it asked for one more
// than this server will hold, which is the same class of answer as a spent
// work budget.
enum class ResourceDetail : std::uint16_t {
    kStatementLimit = 1,
    kPortalLimit = 2,
    // A portal held open past the idle timeout (§10, KW-D3): the executor
    // cursor it pinned was released, so the handle no longer names
    // anything. Distinguished from `kUnknownPortal` because the client's
    // fix is different - it did hold this portal, and it was too slow.
    kPortalIdleTimeout = 3,
};

// ---- The mapping ---------------------------------------------------------

// The wire category for an engine code. Total over `StatusCode`: a code
// with no entry is a compile-time gap, not a runtime `kInternal`, which is
// why this is a switch and not a table lookup with a default.
//
// `StatusCode::kOk` has no category - it is not an error - and answering
// one would let a success be framed as a failure. Callers hold a failed
// `Status` by construction; the function reports `kInternal` for `kOk`
// rather than crashing, and `ErrorFromStatus` refuses it outright.
ErrorCategory CategoryOf(StatusCode code) noexcept;

// The stable name of a category, for the golden list and for logs. Names
// are as frozen as the numbers: a renamed category reads as a new one in
// the guard.
std::string_view ErrorCategoryName(ErrorCategory category) noexcept;

// One decoded `S_ERROR`. The optional halves are §11's `detail str?` and
// `position u32?`: absent is not the empty string and not zero, because
// byte 0 is a real position and "" is a real (if useless) detail.
struct WireError {
    std::uint32_t code = 0;
    bool retryable = false;
    Severity severity = Severity::kError;
    std::string message;
    std::optional<std::string> detail;
    std::optional<std::uint32_t> position;

    ErrorCategory category() const noexcept {
        return static_cast<ErrorCategory>(code >> 16);
    }
    std::uint16_t detail_code() const noexcept {
        return static_cast<std::uint16_t>(code & 0xFFFFu);
    }
};

// An engine failure as a wire error. `retryable` comes from
// `kds::IsRetryable` and from nowhere else (R1).
//
// **The position rides the message, because that is where the engine puts
// it.** Every refusal in this engine "carries the byte position of the
// offending token" (CLAUDE.md) *inside its message text*, and there is no
// structured field to lift. Re-parsing the message to find one would be a
// second reading of a string the message owns, which is the drift this
// project refuses everywhere else - so `position` is filled only by callers
// that hold one as a number, which today is the protocol layer alone.
WireError ErrorFromStatus(const Status& status, std::uint16_t detail = kNoDetail);

// A protocol-layer error, which has no `Status` behind it: a malformed
// frame is not something the engine failed to do (kwp.hpp's note on why
// `kProtocol` exists at all).
WireError ProtocolError(ProtocolDetail detail, std::string message, Severity severity);

// The `S_ERROR` payload:
//
//     code u32, retryable u8, severity u8,
//     message  {u32 len, bytes}
//     detail   {u32 len, bytes}   len 0xFFFFFFFF = absent
//     position u32                0xFFFFFFFF = absent
//
// `0xFFFFFFFF` as absent in both, which is the `{i32 len | -1 = NULL}`
// convention row values already use (§6's "one NULL convention
// everywhere") spelled for a payload field rather than a cell. A message
// is never absent - an error with nothing to say is a defect - so it has
// no such encoding.
std::vector<std::byte> EncodeError(const WireError& error);
StatusOr<WireError> DecodeError(std::span<const std::byte> payload);

}  // namespace kds::wire
