#include <aloe/core>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace {

    using aloe::core::siphash24;
    using aloe::core::SipHashKey;

    // The reference implementation's key, the bytes 00 01 .. 0f.
    constexpr SipHashKey key{.k0 = 0x0706'0504'0302'0100U, .k1 = 0x0f0e'0d0c'0b0a'0908U};

    constexpr std::array<std::byte, 64> counting = [] {
        std::array<std::byte, 64> bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(index);
        }
        return bytes;
    }();

    // The reference implementation's message of `n` bytes, 00 01 .. n-1.
    constexpr std::span<const std::byte> message(const std::size_t n) {
        return std::span{counting}.first(n);
    }

    struct Vector {
        std::size_t length  = 0;
        std::uint64_t value = 0;
    };

    // n = 15 is the SipHash paper's example. The lengths cover an empty message, a tail alone, whole words alone and
    // whole words with a tail.
    constexpr Vector vectors[] = {
        {0,  0x726f'db47'dd0e'0e31U},
        {1,  0x74f8'39c5'93dc'67fdU},
        {8,  0x93f5'f579'9a93'2462U},
        {15, 0xa129'ca61'49be'45e5U},
        {16, 0x3f2a'cc7f'57c2'9bdbU},
        {63, 0x958a'324c'eb06'4572U},
    };

}  // namespace

TEST(SipHash, MatchesTheReferenceVectors) {
    for (const Vector& vector : vectors) {
        EXPECT_EQ(siphash24(key, message(vector.length)), vector.value) << vector.length << " bytes";
    }
}

TEST(SipHash, IsConstexpr) {
    static_assert(siphash24(key, message(0)) == 0x726f'db47'dd0e'0e31U);
}

TEST(SipHash, AnotherKeyGivesAnotherHash) {
    EXPECT_NE(siphash24({.k0 = 1}, message(15)), siphash24({.k0 = 2}, message(15)));
}
