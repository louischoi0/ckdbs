#include "kds/storage/crc32c.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::storage {
namespace {

std::span<const std::byte> AsBytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

TEST(Crc32cTest, MatchesStandardCheckVector) {
    EXPECT_EQ(Crc32c(AsBytes("123456789")), kCrc32cCheckValue);
    EXPECT_EQ(Crc32cExtendSoftware(kCrc32cInit, AsBytes("123456789")), kCrc32cCheckValue);
}

TEST(Crc32cTest, EmptyInputLeavesCrcUnchanged) {
    EXPECT_EQ(Crc32c({}), kCrc32cInit);
    EXPECT_EQ(Crc32cExtend(0x12345678u, {}), 0x12345678u);
}

// The property the page-header codec relies on: it CRCs the page in three
// segments (before the checksum field, four zero bytes, after the field)
// and must get the same answer as one pass over the equivalent buffer.
TEST(Crc32cTest, ExtendIsAssociativeAcrossSplits) {
    const std::string_view whole = "the quick brown fox jumps over the lazy dog";
    const std::uint32_t one_shot = Crc32c(AsBytes(whole));

    for (std::size_t split = 0; split <= whole.size(); ++split) {
        std::uint32_t crc = Crc32cExtend(kCrc32cInit, AsBytes(whole.substr(0, split)));
        crc = Crc32cExtend(crc, AsBytes(whole.substr(split)));
        EXPECT_EQ(crc, one_shot) << "split at " << split;
    }
}

// page.md section 18-5: the hardware and software paths must agree bit for
// bit. Sizes chosen to straddle the hardware path's 8-byte main loop and
// its byte-at-a-time tail.
TEST(Crc32cTest, HardwareAndSoftwareAgree) {
    if (!Crc32cHardwareAvailable()) {
        GTEST_SKIP() << "no SSE4.2 CRC32C on this build/CPU";
    }

    std::vector<std::byte> data;
    data.reserve(1024);
    for (std::size_t i = 0; i < 1024; ++i) {
        data.push_back(static_cast<std::byte>((i * 31 + 7) & 0xFF));
        const std::span<const std::byte> view(data);
        EXPECT_EQ(Crc32cExtendHardware(kCrc32cInit, view), Crc32cExtendSoftware(kCrc32cInit, view))
            << "length " << data.size();
    }
}

TEST(Crc32cTest, DetectsSingleBitFlip) {
    std::array<std::byte, 64> data{};
    const std::uint32_t base = Crc32c(data);
    data[37] = std::byte{0x01};
    EXPECT_NE(Crc32c(data), base);
}

}  // namespace
}  // namespace kds::storage
