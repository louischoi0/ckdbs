#include "kds/storage/keystone.hpp"

#include <string>

namespace kds {

namespace {

std::uint64_t LoadLe64(const std::byte* in) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[i]));
    }
    return v;
}

}  // namespace

StatusOr<std::uint64_t> Keystone::Encode(std::uint64_t id, std::uint8_t flags,
                                          std::uint8_t reserved) {
    if (id > kMaxKeystoneId) {
        return Status::InvalidArgument("Keystone id " + std::to_string(id) +
                                       " exceeds the id space (max " +
                                       std::to_string(kMaxKeystoneId) + ")");
    }

    std::uint64_t word = id;
    word |= static_cast<std::uint64_t>(flags) << kKeystoneIdBits;
    word |= static_cast<std::uint64_t>(reserved) << (kKeystoneIdBits + kKeystoneFlagsBits);
    return word;
}

Keystone Keystone::Decode(std::uint64_t word) noexcept {
    constexpr std::uint64_t kFlagsMask = (std::uint64_t{1} << kKeystoneFlagsBits) - 1;
    constexpr std::uint64_t kReservedMask = (std::uint64_t{1} << kKeystoneReservedBits) - 1;
    Keystone out;
    out.id = word & kMaxKeystoneId;
    out.flags = static_cast<std::uint8_t>((word >> kKeystoneIdBits) & kFlagsMask);
    out.reserved = static_cast<std::uint8_t>(
        (word >> (kKeystoneIdBits + kKeystoneFlagsBits)) & kReservedMask);
    return out;
}

StatusOr<std::uint64_t> KeystoneIdOfPayload(std::span<const std::byte> payload) {
    if (payload.size() < kKeystoneWordSize) {
        return Status::Corruption("tuple payload is shorter than its Keystone word");
    }
    return Keystone::Decode(LoadLe64(payload.data())).id;
}

Status RequirePayloadCarries(std::span<const std::byte> payload, std::uint64_t id) {
    auto encoded_id = KeystoneIdOfPayload(payload);
    if (!encoded_id.ok()) return encoded_id.status();
    if (encoded_id.value() != id) {
        return Status::Corruption("tuple's Keystone id " + std::to_string(encoded_id.value()) +
                                  " does not match the id being inserted (" + std::to_string(id) +
                                  ")");
    }
    return Status::OK();
}

}  // namespace kds
