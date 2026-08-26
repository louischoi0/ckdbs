#include "kds/wire/kwp_types.hpp"

// Payload codecs for KWP v0's frames (docs/inflight/in-progress/workplan-kwp-load.md KL01).
// Encode and decode are exact mirrors, and decode trusts nothing: a short
// or malformed payload is InvalidArgument with the frame named, never a
// partially-filled struct - the same posture the frame codec takes one
// layer down.

namespace kds::wire {

std::vector<std::byte> EncodeClientHello(const ClientHello& hello) {
    PayloadWriter w;
    w.U32(kKwpMagic);
    w.U16(hello.max_version);
    w.U16(hello.min_version);
    w.U64(hello.capabilities);
    w.U8(hello.auth_method);
    w.Str(hello.client_name);
    return w.Take();
}

StatusOr<ClientHello> DecodeClientHello(std::span<const std::byte> payload) {
    PayloadReader r(payload);
    const auto magic = r.U32();
    if (!magic.has_value() || magic.value() != kKwpMagic) {
        return Status::InvalidArgument("C_HELLO: bad or missing magic");
    }
    ClientHello out;
    const auto maxv = r.U16();
    const auto minv = r.U16();
    const auto caps = r.U64();
    const auto auth = r.U8();
    auto name = r.Str();
    if (!maxv || !minv || !caps || !auth || !name.has_value()) {
        return Status::InvalidArgument("C_HELLO: truncated payload");
    }
    out.max_version = maxv.value();
    out.min_version = minv.value();
    out.capabilities = caps.value();
    out.auth_method = auth.value();
    out.client_name = std::move(name.value());
    return out;
}

std::vector<std::byte> EncodeLoadBegin(const LoadBegin& begin) {
    PayloadWriter w;
    w.Str(begin.relation);
    w.U16(begin.flags);
    w.U64(begin.declared_rows);
    return w.Take();
}

StatusOr<LoadBegin> DecodeLoadBegin(std::span<const std::byte> payload) {
    PayloadReader r(payload);
    auto relation = r.Str();
    const auto flags = r.U16();
    const auto declared = r.U64();
    if (!relation.has_value() || !flags || !declared) {
        return Status::InvalidArgument("C_LOAD_BEGIN: truncated payload");
    }
    LoadBegin out;
    out.relation = std::move(relation.value());
    out.flags = flags.value();
    out.declared_rows = declared.value();
    return out;
}

StatusOr<LoadChunkHeader> DecodeLoadChunkHeader(PayloadReader& reader) {
    const auto load_id = reader.U64();
    const auto seq = reader.U32();
    const auto rows = reader.U16();
    if (!load_id || !seq || !rows) {
        return Status::InvalidArgument("C_LOAD_CHUNK: truncated header");
    }
    return LoadChunkHeader{load_id.value(), seq.value(), rows.value()};
}

}  // namespace kds::wire
