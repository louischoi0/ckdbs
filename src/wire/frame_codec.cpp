#include "kds/wire/kwp.hpp"

namespace kds::wire {

std::array<std::byte, kFrameHeaderSize> FrameHeader::Encode() const noexcept {
    std::array<std::byte, kFrameHeaderSize> out{};

    // Explicit little-endian byte assembly (docs/spec/protocol.md D5) rather
    // than a memcpy of the struct - host byte order is not assumed
    // portable (see file comment in kwp.hpp).
    out[kLengthOffset + 0] = static_cast<std::byte>(length & 0xFFu);
    out[kLengthOffset + 1] = static_cast<std::byte>((length >> 8) & 0xFFu);
    out[kLengthOffset + 2] = static_cast<std::byte>((length >> 16) & 0xFFu);
    out[kLengthOffset + 3] = static_cast<std::byte>((length >> 24) & 0xFFu);
    out[kTypeOffset] = static_cast<std::byte>(type);
    out[kFlagsOffset] = static_cast<std::byte>(flags);
    out[kReservedOffset + 0] = static_cast<std::byte>(reserved & 0xFFu);
    out[kReservedOffset + 1] = static_cast<std::byte>((reserved >> 8) & 0xFFu);
    return out;
}

StatusOr<FrameHeader> FrameHeader::Decode(std::span<const std::byte> bytes) {
    if (bytes.size() < kFrameHeaderSize) {
        return Status::InvalidArgument("frame header requires at least kFrameHeaderSize bytes");
    }

    auto byte_at = [&](std::size_t i) { return static_cast<std::uint32_t>(bytes[i]); };

    FrameHeader header{};
    header.length = byte_at(kLengthOffset) | (byte_at(kLengthOffset + 1) << 8) |
                     (byte_at(kLengthOffset + 2) << 16) | (byte_at(kLengthOffset + 3) << 24);
    header.type = static_cast<std::uint8_t>(bytes[kTypeOffset]);
    header.flags = static_cast<std::uint8_t>(bytes[kFlagsOffset]);
    header.reserved = static_cast<std::uint16_t>(byte_at(kReservedOffset) |
                                                  (byte_at(kReservedOffset + 1) << 8));
    return header;
}

std::vector<std::byte> EncodeFrame(std::uint8_t type, std::uint8_t flags,
                                    std::span<const std::byte> payload) {
    FrameHeader header{};
    header.length = kMinFrameLength + static_cast<std::uint32_t>(payload.size());
    header.type = type;
    header.flags = flags;
    header.reserved = 0;

    std::array<std::byte, kFrameHeaderSize> header_bytes = header.Encode();

    std::vector<std::byte> out;
    out.reserve(kFrameHeaderSize + payload.size());
    out.insert(out.end(), header_bytes.begin(), header_bytes.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Status FrameDecoder::Feed(std::span<const std::byte> bytes) {
    if (failed_) {
        return Status::Corruption("FrameDecoder is already in a failed state");
    }

    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    // Validate the header (if fully buffered) as soon as possible, even
    // before the rest of the frame arrives - otherwise a peer could force
    // unbounded buffering ahead of ever finding out kMaxFrame was
    // violated.
    if (buffer_.size() >= kFrameHeaderSize) {
        StatusOr<FrameHeader> header = FrameHeader::Decode(buffer_);
        if (!header.ok()) {
            failed_ = true;
            return header.status();
        }
        if (header.value().length < kMinFrameLength) {
            failed_ = true;
            return Status::Corruption("frame length below kMinFrameLength");
        }
        if (header.value().length > kMaxFrame) {
            failed_ = true;
            return Status::Corruption("frame length exceeds kMaxFrame");
        }
    }
    return Status::OK();
}

std::optional<DecodedFrame> FrameDecoder::PopFrame() {
    if (failed_ || buffer_.size() < kFrameHeaderSize) {
        return std::nullopt;
    }

    StatusOr<FrameHeader> header = FrameHeader::Decode(buffer_);
    if (!header.ok()) {
        failed_ = true;
        return std::nullopt;
    }
    const FrameHeader& h = header.value();
    if (h.length < kMinFrameLength || h.length > kMaxFrame) {
        failed_ = true;
        return std::nullopt;
    }

    std::size_t payload_size = h.length - kMinFrameLength;
    std::size_t total_size = kFrameHeaderSize + payload_size;
    if (buffer_.size() < total_size) {
        return std::nullopt;  // frame not fully received yet
    }

    DecodedFrame frame;
    frame.type = h.type;
    frame.flags = h.flags;
    frame.payload.assign(buffer_.begin() + kFrameHeaderSize, buffer_.begin() + total_size);

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
    return frame;
}

}  // namespace kds::wire
