#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief The shard's initial sequence numbers and timestamp offsets: RFC 6528's keyed function of the flow, as
     * Linux's `secure_tcp_seq_and_ts_off` computes it.
     *
     * One SipHash-2-4 of the flow under a key the generator holds: its low 32 bits plus the clock are the initial
     * sequence number, its high 32 bits the timestamp offset. So a flow opened again later starts further on by the
     * time that passed, two flows that differ in any field start at unrelated numbers, and one flow keeps one
     * timestamp offset for as long as the key lives. The key is drawn once per stack, where Linux draws `net_secret`
     * once per boot, and nothing reads it back. A test may name the next initial sequence number (`force`); the
     * offset stays the flow's.
     */
    class IsnGenerator {
    public:
        /// The two numbers a connection opens with.
        struct Numbers {
            wire::TcpSequence isn{};
            std::uint32_t timestamp_offset = 0;
        };

        explicit IsnGenerator(const core::SipHashKey key) noexcept
            : key_{key} {
        }

        /// A key from the kernel's random source. Setup only; throws what `core::random_bytes` throws.
        [[nodiscard]] static core::SipHashKey draw_key() {
            std::array<std::byte, sizeof(core::SipHashKey)> bytes{};
            core::random_bytes(bytes);
            return std::bit_cast<core::SipHashKey>(bytes);
        }

        /**
         * The numbers of the flow `local` to `remote` at `now`; a forced sequence number replaces the computed one
         * once. The clock term is the stamp's nanoseconds since the clock's epoch in units of 64 ns, Linux's
         * `seq_scale`, where RFC 6528 asks for a 4 microsecond timer: Linux's unit, which wraps the sequence space
         * every 275 seconds, over the stack's own stamp, since the brick reads no other clock.
         */
        [[nodiscard]] Numbers numbers(const Endpoint local, const Endpoint remote, const core::TimePoint now) noexcept {
            const std::array<std::byte, flow_size> flow = flow_bytes(local, remote);
            const std::uint64_t hash                    = core::siphash24(key_, flow);
            const auto nanoseconds =
                static_cast<std::uint64_t>(std::chrono::duration_cast<core::Duration>(now.time_since_epoch()).count());
            const auto clock = static_cast<std::uint32_t>(nanoseconds >> 6U);  // modulo 2^32, as the space wraps
            Numbers numbers{
                .isn              = wire::TcpSequence{static_cast<std::uint32_t>(hash)} + clock,
                .timestamp_offset = static_cast<std::uint32_t>(hash >> 32U),
            };
            if (forced_) {
                numbers.isn = *std::exchange(forced_, std::nullopt);
            }
            return numbers;
        }

        void force(const wire::TcpSequence isn) noexcept {
            forced_ = isn;
        }

    private:
        /// Two addresses and two ports.
        static constexpr std::size_t flow_size = 12;

        /// The hash's input: the local address and the remote address, each `to_uint32()` in four bytes, then the
        /// local port and the remote port in two, every field little-endian.
        [[nodiscard]] static constexpr std::array<std::byte, flow_size> flow_bytes(const Endpoint local,
                                                                                   const Endpoint remote) noexcept {
            std::array<std::byte, flow_size> bytes{};
            const auto store = [&bytes](const std::size_t at, const std::uint32_t value, const std::size_t size) {
                for (std::size_t index = 0; index < size; ++index) {
                    bytes[at + index] = static_cast<std::byte>(value >> (8U * index));
                }
            };
            store(0, local.address.to_uint32(), 4);
            store(4, remote.address.to_uint32(), 4);
            store(8, local.port, 2);
            store(10, remote.port, 2);
            return bytes;
        }

        core::SipHashKey key_;
        std::optional<wire::TcpSequence> forced_{};
    };

}  // namespace aloe::tcp::detail
