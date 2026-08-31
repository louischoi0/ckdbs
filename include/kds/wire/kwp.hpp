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

// Sanity ceiling on a frame's `length` field. **Ratified 2026-08-31 as
// KW-D2** (`instructions/v2.7.0/kw-ratification.md`), which confirmed the
// value this header already held rather than moving it.
//
// It is a ceiling on a *declared* length, not a sizing target: it exists so
// a corrupt or hostile `length` is refused before the decoder allocates
// against it. So it wants to sit far above any legitimate frame and far
// below what would let one connection exhaust the server. The largest
// legitimate frame is a row batch, whose target is `kRowBatchTargetBytes`
// (64 KiB, §9's builder), making this 256x the largest thing the server
// intends to send; a per-connection buffer bounded here is affordable.
//
// **It bounds nothing a shipped statement can produce**: the cross-core
// ring's reply cap is 992 bytes. Locally-answered result batches only.
inline constexpr std::uint32_t kMaxFrame = 16u * 1024u * 1024u;

// The size a server aims at when it seals an `S_ROW_BATCH`
// (docs/spec/protocol.md §7, "batch size server-chosen, default target
// <= 64 KiB per frame"). **Ratified 2026-08-31 with KW-D2**, which named it
// as the quantity `kMaxFrame` is a multiple of.
//
// A *target*, not a ceiling: a batch is sealed once appending the next row
// would carry it past this, so one row wider than the target still ships
// whole - the alternative is a row nothing can ever send. `kMaxFrame` is
// the ceiling, 256x above.
//
// **Deliberately not `kStepBatchTargetBytes`** (server/step_pipeline.hpp,
// 32 KiB). They look like one quantity and are two: the cross-core target
// is bounded by the ring slot it must fit inside - a bound that layer
// derives from the transport, after a batch 32x the slot vanished
// silently - and this one is bounded by nothing but the frame. Naming
// them once would tie a wire frame's size to a ring's slot, which is the
// coupling that defect came from rather than the fix for it.
inline constexpr std::size_t kRowBatchTargetBytes = 64u * 1024u;

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

// **One registry per direction, and this is it** (unified 2026-08-31,
// milestone KW). `kwp_types.hpp` held a second pair of enums for the v0
// load endpoint - `ClientFrame`/`ServerFrame` - whose base block collided
// with this one on five values: client 2/3 (kParse/kBind there kPing/
// kTerminate) and server 2/3/4 (kReady/kParseOk/kBindOk there kError/
// kComplete/kPong). Two numbering spaces on one `type` byte is not a
// naming problem: a frame's meaning would have depended on which endpoint
// received it, and the two endpoints are the same protocol. That file's
// own comment had already promised the base block to "the spec's own
// ordering when it lands", so the load block moved onto this numbering
// rather than the reverse, and its four in-tree speakers moved with it.
// The load frames are appended here at 16+, the block they already held.
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
    // The authentication sub-frame (§3's `[PROPOSED shape]`, proposed and
    // built 2026-08-31 as P07). One frame in each direction carrying one
    // SCRAM message, which is what §14 committed to: "KWP will carry these
    // same SCRAM message bodies in handshake frames; only this line
    // framing is protocol-specific" (`server/auth.hpp`). It takes the last
    // base-block number because it is the last frame the base block will
    // need - every other §4 frame is already numbered above.
    kAuth = 15,
    // The bulk-load block, gated by `kCapBulkLoad` (kwp_types.hpp): the
    // capability bit is bit 16 and the frames start at 16, so a reader of
    // either can find the other.
    kLoadBegin = 16,
    kLoadChunk,
    kLoadEnd,
    kLoadAbort,
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
    kAuth = 13,  // the server half of the exchange above
    kLoadReady = 16,
    kLoadAck,
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
// Per-transaction protocol field, carried by `C_TXN_BEGIN`. The numbering
// mirrors `docs/spec/wal.md` §1's D1/D2/D3 - **confirmed 2026-08-31**, when
// that document and `wal::DurabilityClass` both existed to be checked
// against: `kStrict`/`kGroup`/`kRelaxed` are 1/2/3 in both, so the wire
// value is the engine value and `DurabilityClassOf` below is a range check
// rather than a translation table.
//
// Zero is the wire's own value and has no engine counterpart: it means
// "whatever this session's default is", which is a statement about the
// session and not about a class (`Session::EffectiveDurability`).
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
    // Appended for StatusCode::kFkViolation and kAssertionViolation
    // (2026-08-31, P12). Both earn a category on the rule above and on a
    // fact the newline protocol already established: `ErrorReply` gives
    // each of them a token a client switches on (`FK_VIOLATION`,
    // `ASSERTION_VIOLATION`), so folding either into kInvalidArgument here
    // would make the binary protocol *less* discriminating than the text
    // one it replaces - and a client library ported across would lose a
    // branch it already has.
    kFkViolation,
    kAssertionViolation,
};

// Packs a wire error code as `category u16 << 16 | detail u16` (spec
// §11). Detail codes are append-only per category; this header does not
// define a detail taxonomy yet (skeleton only, per docs/inflight/in-progress/protocol-wp.md P05).
constexpr std::uint32_t MakeErrorCode(ErrorCategory category, std::uint16_t detail) noexcept {
    return (static_cast<std::uint32_t>(category) << 16) | detail;
}

// ---- Handshake payloads (docs/spec/protocol.md §3) ------------------------------
// **They live in kwp_types.hpp, with their codecs** (moved 2026-08-31).
// This header declared `ClientHelloPayload`/`ServerHelloPayload` as a
// decoded-form-only sketch while `kwp_types.hpp` held a `ClientHello` that
// an endpoint actually encoded and decoded - two names for one frame's
// payload, of which only one was ever on a wire. The sketch is gone;
// `ClientHello`/`ServerHello` are the payloads, `EncodeClientHello` and
// friends are their codecs, and the *negotiation* over them is
// `wire/handshake.hpp` (P07). One struct, one codec, one place.

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
