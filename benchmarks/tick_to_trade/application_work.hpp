#pragma once

#include <aloe/stream>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <round_config.hpp>

namespace aloe::benchmarks {

    inline constexpr std::uint64_t fold_seed  = std::uint64_t{0xcbf29ce484222325};  ///< FNV-1a's offset basis.
    inline constexpr std::uint64_t fold_prime = std::uint64_t{0x100000001b3};

    /// The round number a tick or an order opens with: `round_prefix` bytes, little-endian.
    [[nodiscard]] inline std::uint64_t read_round(const std::span<const std::byte> payload) noexcept {
        assert(payload.size() >= round_prefix && "a payload opens with the round number");
        std::uint64_t round = 0;
        for (std::size_t index = 0; index < round_prefix; ++index) {
            round |= std::uint64_t{std::to_integer<std::uint8_t>(payload[index])} << (8U * index);
        }
        return round;
    }

    inline void write_round(const std::span<std::byte> payload, const std::uint64_t round) noexcept {
        assert(payload.size() >= round_prefix && "a payload opens with the round number");
        for (std::size_t index = 0; index < round_prefix; ++index) {
            payload[index] = static_cast<std::byte>((round >> (8U * index)) & std::uint64_t{0xff});
        }
    }

    /// A fold over every byte, FNV-1a: the stand-in for decoding the whole message. The caller keeps the result, so
    /// the compiler keeps the work.
    [[nodiscard]] inline std::uint64_t fold(const std::span<const std::byte> bytes,
                                            const std::uint64_t seed = fold_seed) noexcept {
        std::uint64_t accumulator = seed;
        for (const std::byte byte : bytes) {
            accumulator = (accumulator ^ std::to_integer<std::uint64_t>(byte)) * fold_prime;
        }
        return accumulator;
    }

    /// The same fold over the chunks of a read view, in order: what `unread()` gives the application.
    template <stream::IsReadView View>
    [[nodiscard]] std::uint64_t fold(const View& view) noexcept {
        std::uint64_t accumulator = fold_seed;
        for (const std::span<const std::byte> chunk : view) {
            accumulator = fold(chunk, accumulator);
        }
        return accumulator;
    }

    /// An order: the round number, then each byte its own index, to `order_size`.
    inline void write_order(const std::span<std::byte> order, const std::uint64_t round) noexcept {
        assert(order.size() == order_size && "an order is order_size bytes");
        write_round(order, round);
        for (std::size_t index = round_prefix; index < order.size(); ++index) {
            order[index] = static_cast<std::byte>(index);
        }
    }

    /// A tick's payload of `size` bytes with the same layout as an order. The peer builds it, off the clock.
    [[nodiscard]] inline std::vector<std::byte> tick_payload(const std::size_t size, const std::uint64_t round) {
        assert(size >= round_prefix && "a tick carries at least the round number");
        std::vector<std::byte> payload(size);
        write_round(payload, round);
        for (std::size_t index = round_prefix; index < size; ++index) {
            payload[index] = static_cast<std::byte>(index);
        }
        return payload;
    }

}  // namespace aloe::benchmarks
