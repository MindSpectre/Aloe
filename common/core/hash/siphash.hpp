#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aloe::core {

    /// A SipHash key: bytes 0 to 7 of the 16-byte key are `k0`, bytes 8 to 15 `k1`, both little-endian.
    struct SipHashKey {
        std::uint64_t k0 = 0;
        std::uint64_t k1 = 0;

        friend constexpr bool operator==(const SipHashKey&, const SipHashKey&) noexcept = default;
    };

    namespace detail {

        /// SipHash's four words of state and its round, SipRound, as the reference implementation writes them.
        struct SipState {
            std::uint64_t v0 = 0;
            std::uint64_t v1 = 0;
            std::uint64_t v2 = 0;
            std::uint64_t v3 = 0;

            constexpr void round() noexcept {
                v0 += v1;
                v1  = std::rotl(v1, 13);
                v1 ^= v0;
                v0  = std::rotl(v0, 32);
                v2 += v3;
                v3  = std::rotl(v3, 16);
                v3 ^= v2;
                v0 += v3;
                v3  = std::rotl(v3, 21);
                v3 ^= v0;
                v2 += v1;
                v1  = std::rotl(v1, 17);
                v1 ^= v2;
                v2  = std::rotl(v2, 32);
            }

            /// Takes one message word in with two rounds: the "2" of SipHash-2-4.
            constexpr void compress(const std::uint64_t word) noexcept {
                v3 ^= word;
                round();
                round();
                v0 ^= word;
            }
        };

        /// `bytes`, at most eight of them, as a little-endian word; the bytes missing at the top are zero.
        [[nodiscard]] constexpr std::uint64_t load_le64(const std::span<const std::byte> bytes) noexcept {
            std::uint64_t word = 0;
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                word |= std::to_integer<std::uint64_t>(bytes[index]) << (8U * index);
            }
            return word;
        }

    }  // namespace detail

    /// SipHash-2-4 of `message` under `key`: two compression rounds per eight-byte word, four finalization rounds.
    [[nodiscard]] constexpr std::uint64_t siphash24(const SipHashKey& key,
                                                    const std::span<const std::byte> message) noexcept {
        // The initial state is the key over "somepseudorandomlygeneratedbytes", eight bytes per word.
        detail::SipState state{
            .v0 = key.k0 ^ 0x736f'6d65'7073'6575U,
            .v1 = key.k1 ^ 0x646f'7261'6e64'6f6dU,
            .v2 = key.k0 ^ 0x6c79'6765'6e65'7261U,
            .v3 = key.k1 ^ 0x7465'6462'7974'6573U,
        };

        const std::size_t whole = message.size() - (message.size() % 8);
        for (std::size_t offset = 0; offset < whole; offset += 8) {
            state.compress(detail::load_le64(message.subspan(offset, 8)));
        }
        // The last word: the bytes after the whole words, with the message's length, modulo 256, in its top byte.
        state.compress((static_cast<std::uint64_t>(message.size()) << 56U) | detail::load_le64(message.subspan(whole)));

        state.v2 ^= 0xffU;
        for (int finalization = 0; finalization < 4; ++finalization) {
            state.round();
        }
        return state.v0 ^ state.v1 ^ state.v2 ^ state.v3;
    }

}  // namespace aloe::core
