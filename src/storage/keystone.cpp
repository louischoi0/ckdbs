#include "kds/storage/keystone.hpp"

namespace kds {

StatusOr<std::uint64_t> Keystone::Encode(std::uint64_t id, std::uint8_t flags,
                                          std::uint16_t reserved) {
    if (id > kMaxKeystoneId) {
        return Status::InvalidArgument("Keystone id exceeds 40-bit range");
    }

    std::uint64_t word = id;
    word |= static_cast<std::uint64_t>(flags) << kKeystoneIdBits;
    word |= static_cast<std::uint64_t>(reserved) << (kKeystoneIdBits + kKeystoneFlagsBits);
    return word;
}

Keystone Keystone::Decode(std::uint64_t word) noexcept {
    Keystone out;
    out.id = word & kMaxKeystoneId;
    out.flags = static_cast<std::uint8_t>((word >> kKeystoneIdBits) & 0xFFu);
    out.reserved = static_cast<std::uint16_t>(
        (word >> (kKeystoneIdBits + kKeystoneFlagsBits)) & 0xFFFFu);
    return out;
}

}  // namespace kds
