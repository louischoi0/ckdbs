#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kds/base/status.hpp"

// KWP/1 - the KDS wire protocol (docs/spec/protocol.md).
// This header holds the frame format, frame catalog, capability bits,
// durability levels, and the wire error-code taxonomy, plus the
// FrameHeader codec and FrameDecoder declarations whose bodies live in
// src/wire/frame_codec.cpp (docs/inflight/in-progress/protocol-wp.md P06) - the same
// declare-in-header/implement-in-cpp split as kds::storage::Keystone
// (keystone.hpp/.cpp) and kds::server::TcpServer (tcp_server.hpp/.cpp).
//
// Frame layout (docs/spec/protocol.md §2): every message in both directions is
// one frame: `{length u32 LE, type u8, flags u8, reserved u16, payload}`.
// `length` counts everything after itself (type..payload), i.e.
// `kMinFrameLength + payload.size()`, NOT the 4 `length` bytes themselves.
// Encoding is little-endian end to end (docs/spec/protocol.md D5) regardless
// of host byte order (rules.md §7's platform pin is still [OPEN], so
// nothing here may assume a little-endian host) - hence explicit
// shift/mask byte assembly in the .cpp rather than a host-native memcpy,
// the same "never assume implementation-defined layout" principle
// rules.md §5 applies to bit-packed fields, extended here to multi-byte
// wire integers.

namespace kds::wire {

// ---- Frame header (docs/spec/protocol.md §2) ------------------------------------

inline constexpr std::size_t kLengthOffset = 0;
inline constexpr std::size_t kTypeOffset = 4;
inline constexpr std::size_t kFlagsOffset = 5;
inline constexpr std::size_t kReservedOffset = 6;
inline constexpr std::size_t kFrameHeaderSize = 8;  // payload starts here

// type(1) + flags(1) + reserved(2): every frame's `length` field must be
// at least this even with an empty payload; less than this is a framing
// violation (bad length, resync impossible - docs/spec/protocol.md §2).
inline constexpr std::uint32_t kMinFrameLength = 4;

// Sanity ceiling on a frame's `length` field. [OPEN: default] per spec
// §14 - 16 MiB is this document's starting point, not a confirmed value.
inline constexpr std::uint32_t kMaxFrame = 16u * 1024u * 1024u;

// Decoded form of the 8-byte frame header.
struct FrameHeader {
    std::uint32_t length;
    std::uint8_t type;
    std::uint8_t flags;
    std::uint16_t reserved;  // 0; receivers ignore (spec §2)

    // Packs the header into its 8-byte wire form (explicit little-endian
    // byte assembly - see file comment). Never fails: every FrameHeader
    // value is representable.
    std::array<std::byte, kFrameHeaderSize> Encode() const noexcept;

    // Unpacks a wire header. Fails with InvalidArgument if `bytes` is
    // shorter than kFrameHeaderSize. Does not validate `length` against
    // kMinFrameLength/kMaxFrame - callers (FrameDecoder) apply those
    // framing-level checks, since a bare header decode has no way to
    // know the caller's buffering policy.
    static StatusOr<FrameHeader> Decode(std::span<const std::byte> bytes);
};

// ---- Frame catalog (docs/spec/protocol.md §4) -----------------------------------
// Values are frozen and append-only from the moment any wire client
// exists: never renumber, never reuse a retired value. Client and server
// frame types are separate enums, not a shared numbering space - which
// one applies is determined by which side of the connection is decoding,
// not by the numeric value alone (mirrors how the spec itself lists them
// as two separate catalogs).

enum class ClientFrameType : std::uint8_t {
    kHello = 1,
    kParse,
    kBind,
    kExecute,
    kContinue,
    kDescribe,
    kClose,
    kSync,
    kTxnBegin,
    kTxnCommit,
    kTxnAbort,
    kPing,
    kCancel,
    kTerminate,
};

enum class ServerFrameType : std::uint8_t {
    kHello = 1,
    kReady,
    kParseOk,
    kBindOk,
    kRowDesc,
    kRowBatch,
    kPortalSuspended,
    kComplete,
    kTxnOk,
    kError,
    kPong,
    kNotice,
};

// ---- Capabilities (docs/spec/protocol.md §3) ------------------------------------
// Bitset negotiated during the handshake; the session's active set is
// the intersection of the client's and server's bits.

enum class Capability : std::uint64_t {
    kStreaming = std::uint64_t{1} << 0,
    kCancel = std::uint64_t{1} << 1,
    kTlsRequired = std::uint64_t{1} << 2,
    kCompression = std::uint64_t{1} << 3,  // [OPEN] - shape not yet specified
};

// ---- Durability (docs/spec/protocol.md §9) --------------------------------------
// Per-transaction protocol field. Numbering is meant to mirror
// docs/spec/wal.md §1's D1/D2/D3 durability classes, but that document does
// not exist yet - treat this enum as the
// authoritative numbering until docs/spec/wal.md lands and either confirms or
// amends it.
enum class DurabilityLevel : std::uint8_t {
    kSessionDefault = 0,
    kStrict = 1,   // D1
    kGroup = 2,    // D2
    kRelaxed = 3,  // D3
};

// ---- Error taxonomy (docs/spec/protocol.md §11) ---------------------------------
// Wire-level error categories: deliberately a superset of
// kds::StatusCode (kds/base/status.hpp). kInternal/kProtocol/kCancelled
// have no engine-level Status equivalent - they are wire/session-only
// categories that can occur before or outside any engine call (e.g. a
// malformed frame is kProtocol, not any kind of Status). kUnsupported was
// in that list until docs/spec/parser-v2.md I18 gave the language a use for it
// at the engine level; StatusCode::kUnsupported now exists and maps here.
// The Status -> ErrorCategory mapping table for the categories that DO
// overlap is not part of this header; it lands with the error registry
// (docs/inflight/in-progress/protocol-wp.md P12, src/wire/error_registry.cpp), including its
// append-only golden-list guard.
//
// kTxnConflict has an engine-level equivalent:
// StatusCode::kTxnConflict maps here with **retryable = 1**, which
// docs/spec/protocol.md §11 makes part of the compatibility surface. It is the
// only retryable category (kds::IsRetryable is the engine-side spelling of
// the same fact), so P12's registry must not derive the bit per category by
// hand - it should ask IsRetryable for the codes that map from a Status.
enum class ErrorCategory : std::uint16_t {
    kInvalidArgument = 0,
    kNotFound,
    kAlreadyExists,
    kOutOfSpace,
    kOutOfRange,
    kCorruption,
    kIoError,
    kInternal,
    kProtocol,
    kUnsupported,
    kTxnConflict,
    kCancelled,
    // Appended for StatusCode::kCardinalityViolation. Spec §11 says
    // categories "mirror engine Status" over an open-ended list, so a new
    // engine code earns a category rather than being folded into
    // kInvalidArgument - a client cannot fix a cardinality violation by
    // fixing its arguments. Appended at the end and never renumbered:
    // the numbering is the compatibility surface.
    kCardinalityViolation,
    // Appended for StatusCode::kResourceExhausted, same reasoning as
    // above: a client cannot fix a spent work budget by fixing its
    // arguments, so folding it into kInvalidArgument would misdirect.
    kResourceExhausted,
    // Appended for StatusCode::kUnknownOutcome (SS1,
    // server/statement_ship_service.hpp). It earns a category more
    // clearly than either above it: every other category in this list
    // means the statement did not take effect, and this one means nobody
    // can say whether it did. A client that cannot tell it apart cannot
    // write a correct retry loop, which is the whole reason categories
    // mirror engine Status rather than being coarsened.
    kUnknownOutcome,
};

// Packs a wire error code as `category u16 << 16 | detail u16` (spec
// §11). Detail codes are append-only per category; this header does not
// define a detail taxonomy yet (skeleton only, per docs/inflight/in-progress/protocol-wp.md P05).
constexpr std::uint32_t MakeErrorCode(ErrorCategory category, std::uint16_t detail) noexcept {
    return (static_cast<std::uint32_t>(category) << 16) | detail;
}

// ---- Handshake payloads (docs/spec/protocol.md §3) ------------------------------
// Decoded, in-memory form only. Unlike FrameHeader these are NOT
// fixed-size wire structs - both carry a variable-length string field, so
// they get no static_assert offsets here. Wire encode/decode for these
// lands with the handshake state machine (docs/inflight/in-progress/protocol-wp.md P07,
// src/wire/handshake.cpp), not this header.

struct ClientHelloPayload {
    std::uint32_t magic;         // 'KWP1'
    std::uint16_t max_version;
    std::uint16_t min_version;
    std::uint64_t capabilities;  // bitset of Capability
    std::uint8_t auth_method;    // v1: NONE only; SCRAM_SHA256/MTLS reserved
    std::string client_info;     // telemetry only, never interpreted
};

struct ServerHelloPayload {
    std::uint16_t version;
    std::uint64_t capabilities;  // intersection of client/server bits
    std::uint64_t session_id;
    std::uint64_t cancel_key;  // random per session (spec §10)
    std::string server_info;
};

// ---- Frame codec (docs/inflight/in-progress/protocol-wp.md P06; bodies in frame_codec.cpp) -----

// One fully-received frame, as produced by FrameDecoder::PopFrame().
struct DecodedFrame {
    std::uint8_t type;
    std::uint8_t flags;
    std::vector<std::byte> payload;
};

// Encodes a complete frame (header + payload) ready to write to a
// socket. `header.length`/`header.reserved` are computed here; callers
// only supply `type`/`flags`/`payload`.
std::vector<std::byte> EncodeFrame(std::uint8_t type, std::uint8_t flags,
                                    std::span<const std::byte> payload);

// Accumulates arbitrary byte chunks from a stream (TCP has no message
// boundaries - a chunk may split a frame anywhere, including mid-header)
// and yields complete frames as they close. Not thread-safe; one decoder
// per connection.
//
// Internal buffering here is a plain growing std::vector with front
// erasure on each popped frame - correctness-first for this standalone
// codec. The zero-allocation steady-state discipline docs/spec/sched.md and
// docs/inflight/in-progress/protocol-wp.md's standing instructions require is a property of
// the *reactor integration* (docs/inflight/in-progress/protocol-wp.md P13), not this class in
// isolation; revisit buffer reuse when this is wired into the real
// per-connection I/O path.
class FrameDecoder {
public:
    // Feeds newly-arrived bytes. Returns Corruption once a frame's
    // declared `length` is seen to violate kMinFrameLength/kMaxFrame -
    // checked as soon as a full header is buffered, even if the rest of
    // the frame hasn't arrived yet, so a hostile/broken peer can't force
    // unbounded buffering before the violation is caught. Once failed(),
    // every subsequent Feed() keeps failing without inspecting `bytes`:
    // per docs/spec/protocol.md §2, "on framing-level corruption where resync
    // is impossible, connection close" - a caller must not be able to
    // resync on garbage after this point.
    Status Feed(std::span<const std::byte> bytes);

    // Pops one fully-received frame, if any is buffered (FIFO order).
    // Returns std::nullopt if failed() or no complete frame is available
    // yet - never crashes on a partial buffer.
    std::optional<DecodedFrame> PopFrame();

    bool failed() const noexcept { return failed_; }

private:
    std::vector<std::byte> buffer_;
    bool failed_ = false;
};

}  // namespace kds::wire
