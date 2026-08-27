#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"

// KWP v0's frame-type registry and the payload codecs of the frames it
// uses (docs/inflight/in-progress/workplan-kwp-load.md KW2, docs/spec/protocol.md §4,
// docs/spec/bulkinsert.md §3.1).
//
// **The first concrete numbers on the wire, and append-only forever** -
// the sys.access_stats rule, on a surface clients compile against: a
// value here may never change meaning, and a new frame takes the next
// free number in its block however the spec orders its prose. Client and
// server types are separate enums per kwp.hpp's standing comment - the
// direction disambiguates, so the two spaces never constrain each other.
//
// The 16+ block belongs to the BULK_LOAD capability. The base block
// (2..15) is deliberately left sparse so the query surface (C_PARSE,
// C_BIND, ... - protocol.md §4's full list) can take the spec's own
// ordering when it lands; v0 assigns only what it speaks.

namespace kds::wire {

inline constexpr std::uint32_t kKwpMagic = 0x3150574Bu;  // 'KWP1' LE
inline constexpr std::uint16_t kKwpVersion = 1;

// Capability bits (C_HELLO/S_HELLO `capabilities u64`). Bit 16 opens the
// load block, matching the frame numbering: capability and frames move
// together or not at all.
inline constexpr std::uint64_t kCapBulkLoad = 1ull << 16;

enum class ClientFrame : std::uint8_t {
    kHello = 1,
    kPing = 2,
    kTerminate = 3,
    kLoadBegin = 16,
    kLoadChunk = 17,
    kLoadEnd = 18,
    kLoadAbort = 19,
};

enum class ServerFrame : std::uint8_t {
    kHello = 1,
    kError = 2,
    kComplete = 3,
    kPong = 4,
    kLoadReady = 16,
    kLoadAck = 17,
};

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
    // Length-prefixed (u16) UTF-8, the protocol's string shape.
    void Str(std::string_view s) {
        U16(static_cast<std::uint16_t>(s.size()));
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        bytes_.insert(bytes_.end(), p, p + s.size());
    }
    std::vector<std::byte> Take() { return std::move(bytes_); }

private:
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

// ---- v0 frame payloads (KW3, KW4) ----------------------------------------

struct ClientHello {
    std::uint16_t max_version = kKwpVersion;
    std::uint16_t min_version = kKwpVersion;
    std::uint64_t capabilities = 0;
    std::uint8_t auth_method = 0;  // v0: NONE only
    std::string client_name;       // telemetry only
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

std::vector<std::byte> EncodeLoadBegin(const LoadBegin& begin);
StatusOr<LoadBegin> DecodeLoadBegin(std::span<const std::byte> payload);

// The chunk header alone; `rest` in the reader is the row bytes.
StatusOr<LoadChunkHeader> DecodeLoadChunkHeader(PayloadReader& reader);

}  // namespace kds::wire
