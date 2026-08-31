#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/wire/kwp.hpp"

// KWP v0's frame-type registry and the payload codecs of the frames it
// uses (docs/inflight/in-progress/workplan-kwp-load.md KW2, docs/spec/protocol.md §4,
// docs/spec/bulkinsert.md §3.1).
//
// **The frame numbering left this file on 2026-08-31** (milestone KW).
// It held `ClientFrame`/`ServerFrame` for the load endpoint alongside
// `kwp.hpp`'s `ClientFrameType`/`ServerFrameType` for the query surface,
// and the two collided on five values in the base block. The promise made
// here - "the base block is deliberately left sparse so the query surface
// can take the spec's own ordering when it lands" - is what the merge
// kept: the query surface took the base block as `kwp.hpp` numbered it,
// and the load block moved to `kwp.hpp` at the 16+ numbers it already
// held. The five that collided (`kPing`/`kTerminate` client,
// `kError`/`kComplete`/`kPong` server) changed value, which is why this
// happened before a second endpoint existed rather than after.
//
// What stays here is what it always was minus the enums: the capability
// bit, the payload readers and writers, and the v0 load payloads.

namespace kds::wire {

inline constexpr std::uint32_t kKwpMagic = 0x3150574Bu;  // 'KWP1' LE
inline constexpr std::uint16_t kKwpVersion = 1;

// Capability bits (C_HELLO/S_HELLO `capabilities u64`). Bit 16 opens the
// load block, matching the frame numbering: capability and frames move
// together or not at all.
inline constexpr std::uint64_t kCapBulkLoad = 1ull << 16;

// ---- Little-endian payload helpers ---------------------------------------
// The frame codec frames; these read and write *inside* a payload. All
// bounds-checked reads answer nullopt rather than trusting a length a
// client declared (kwp.hpp's rule, one layer down).

class PayloadWriter {
public:
    void U8(std::uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }
    void U16(std::uint16_t v) { Raw(&v, 2); }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void U64(std::uint64_t v) { Raw(&v, 8); }
    // ---- Two string shapes, and the rule that picks one ----------------
    //
    // `Str` is `{u16 len, bytes}`: **names**. A relation, a column, a
    // statement or portal handle, a client's telemetry string - every one
    // of them is short by construction, and the row description carries
    // one per field, which makes this the byte the protocol sends most.
    // Paying four bytes there to buy a length nothing can reach is the
    // wrong trade.
    //
    // `Text` is `{u32 len, bytes}`: **content**. SQL statement text, an
    // error message, an error's detail. These have no natural bound short
    // of `kMaxFrame`, and a 64 KiB ceiling on a statement would be a
    // limit invented by a length field - a bulk `INSERT ... VALUES` can
    // exceed it, and refusing one for that reason would be indefensible.
    //
    // `docs/spec/protocol.md` §2 says both, since 2026-08-31; it used to
    // say `{u32 len, bytes}` for every variable-length field, which no
    // frame ever did - the v0 hello and the row description were u16 from
    // the day they were written.
    void Str(std::string_view s) {
        U16(static_cast<std::uint16_t>(s.size()));
        Bytes(s);
    }
    void Text(std::string_view s) {
        U32(static_cast<std::uint32_t>(s.size()));
        Bytes(s);
    }
    // A `Text` field that is absent, as distinct from present and empty:
    // 0xFFFFFFFF, the `{i32 len | -1 = NULL}` convention row values use
    // (§6's "one NULL convention everywhere"), spelled for a payload
    // field.
    void AbsentText() { U32(0xFFFFFFFFu); }
    std::vector<std::byte> Take() { return std::move(bytes_); }

private:
    void Bytes(std::string_view s) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        bytes_.insert(bytes_.end(), p, p + s.size());
    }
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        bytes_.insert(bytes_.end(), b, b + n);  // LE host assumed, rules.md's
                                                // portability note applies at
                                                // the codec boundary once.
    }
    std::vector<std::byte> bytes_;
};

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    std::optional<std::uint8_t> U8() { return Take<std::uint8_t>(); }
    std::optional<std::uint16_t> U16() { return Take<std::uint16_t>(); }
    std::optional<std::uint32_t> U32() { return Take<std::uint32_t>(); }
    std::optional<std::uint64_t> U64() { return Take<std::uint64_t>(); }
    std::optional<std::string> Str() {
        auto n = U16();
        if (!n.has_value() || bytes_.size() - at_ < n.value()) return std::nullopt;
        std::string out(reinterpret_cast<const char*>(bytes_.data() + at_), n.value());
        at_ += n.value();
        return out;
    }
    // The `Text` counterpart of `Str`, and its absent form: nullopt for a
    // truncated field, and an engaged optional holding nullopt for the
    // 0xFFFFFFFF that means "not present". Two levels because the two
    // failures are different - a client that omitted an optional field and
    // a client that lied about a length must not be answered alike.
    std::optional<std::optional<std::string>> Text() {
        auto n = U32();
        if (!n.has_value()) return std::nullopt;
        if (n.value() == 0xFFFFFFFFu) return std::optional<std::string>{};
        if (bytes_.size() - at_ < n.value()) return std::nullopt;
        std::string out(reinterpret_cast<const char*>(bytes_.data() + at_), n.value());
        at_ += n.value();
        return std::optional<std::string>{std::move(out)};
    }

    // The unread remainder - a chunk's row bytes, handed whole to the row
    // codec rather than re-copied.
    std::span<const std::byte> Rest() const { return bytes_.subspan(at_); }
    bool Exhausted() const { return at_ == bytes_.size(); }

private:
    template <typename T>
    std::optional<T> Take() {
        if (bytes_.size() - at_ < sizeof(T)) return std::nullopt;
        T v;
        std::memcpy(&v, bytes_.data() + at_, sizeof(T));
        at_ += sizeof(T);
        return v;
    }
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

// ---- Authentication methods (`C_HELLO.auth_method`, §3) ------------------
//
// A `u8` on the wire and an enum here. `kNone` is what a v0 client sends
// and what an unauthenticated server admits; `kScramSha256` runs the
// exchange `server/auth.hpp` describes, carried in `C_AUTH`/`S_AUTH`
// frames. `kMtls` is reserved and refused: the transport already
// authenticates in that design, so the method needs a decision about what
// the *session's* identity then is, and §14 has not taken one.
inline constexpr std::uint8_t kAuthNone = 0;
inline constexpr std::uint8_t kAuthScramSha256 = 1;
inline constexpr std::uint8_t kAuthMtls = 2;

// ---- Handshake payloads (§3) ---------------------------------------------

struct ClientHello {
    std::uint16_t max_version = kKwpVersion;
    std::uint16_t min_version = kKwpVersion;
    std::uint64_t capabilities = 0;
    std::uint8_t auth_method = kAuthNone;
    std::string client_name;  // telemetry only, never interpreted
};

// The server's answer. `session_id` and `cancel_key` are minted by the
// endpoint and passed in, never generated here: randomness is an injected
// concern (rules.md §4), and a negotiation that produced its own would be
// untestable byte-for-byte - which is exactly what the golden sessions
// (P16) need it to be.
struct ServerHello {
    std::uint16_t version = kKwpVersion;
    std::uint64_t capabilities = 0;  // the intersection, never the offer
    std::uint64_t session_id = 0;
    std::uint64_t cancel_key = 0;  // §10; a wrong one is silently ignored
    std::string server_info;
};

struct LoadBegin {
    std::string relation;
    std::uint16_t flags = 0;         // reserved 0
    std::uint64_t declared_rows = 0; // 0 = unknown; informational only
};

struct LoadChunkHeader {
    std::uint64_t load_id = 0;
    std::uint32_t chunk_seq = 0;
    std::uint16_t row_count = 0;
    // Row bytes follow, in the D5 encoding the S_LOAD_READY descriptors
    // announced (wire/row_codec.hpp).
};

std::vector<std::byte> EncodeClientHello(const ClientHello& hello);
StatusOr<ClientHello> DecodeClientHello(std::span<const std::byte> payload);

std::vector<std::byte> EncodeServerHello(const ServerHello& hello);
StatusOr<ServerHello> DecodeServerHello(std::span<const std::byte> payload);

std::vector<std::byte> EncodeLoadBegin(const LoadBegin& begin);
StatusOr<LoadBegin> DecodeLoadBegin(std::span<const std::byte> payload);

// The chunk header alone; `rest` in the reader is the row bytes.
StatusOr<LoadChunkHeader> DecodeLoadChunkHeader(PayloadReader& reader);

}  // namespace kds::wire
