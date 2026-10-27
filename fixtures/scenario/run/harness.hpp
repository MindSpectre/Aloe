#pragma once

#include <aloe/core>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aloe::scenario {

    struct HarnessConfig {
        wire::MacAddress stack_mac{0x02, 0, 0, 0, 0, 0x01};
        wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
        wire::Ipv4Address stack_address{10, 0, 0, 2};
        wire::Ipv4Address peer_address{10, 0, 0, 1};
        std::uint8_t prefix      = 24;
        std::uint16_t stack_port = 8080;   ///< The port `bind` binds and `listen` listens on.
        std::uint16_t peer_port  = 40000;  ///< The peer's port: the source of inbound frames, the target of `connect`.
        wire::TcpSequence peer_isn{1'000'000};  ///< Not zero, so a stack that assumed zero would fail.
        fabric::EmulatedOffloads offloads = fabric::EmulatedOffloads::None;
        tcp::TcpConfig tcp{};
        core::Duration resolution   = std::chrono::milliseconds{1};  ///< The wheel's, and the runner's step.
        std::size_t pool_size       = 256;
        core::Duration arp_lifetime = std::chrono::hours{1};  ///< `arp_reachable`; `arp_expire` is twice it.
    };

    /// One frame the peer's port received, parsed, with the verdict on each checksum the stack wrote.
    struct Collected {
        frames::ParsedFrame frame;
        bool ipv4_checksum_ok = false;
        bool tcp_checksum_ok  = false;
    };

    /**
     * @brief The TCP brick over queue 0 of a fabric port, and the peer's port on the same fabric.
     *
     * Both MACs are learned, so no ARP is needed. The runner composes `inject`, `step`, `flush` and `collect`.
     */
    class Harness {
    public:
        using Packet     = fabric::Packet;
        using Ip         = net::Ipv4<fabric::Port>;
        using Tcp        = tcp::Stack<Ip>;
        using Connection = Tcp::ConnectionType;
        using Event      = Tcp::EventType;

        /// Both MACs learned; nothing listening.
        explicit Harness(const HarnessConfig& config);

        [[nodiscard]] const HarnessConfig& config() const noexcept {
            return config_;
        }

        [[nodiscard]] fabric::Fabric& fabric() noexcept {
            return fabric_;
        }

        [[nodiscard]] fabric::Port& stack_port() noexcept {
            return *stack_port_;
        }

        [[nodiscard]] fabric::Port& peer_port() noexcept {
            return *peer_port_;
        }

        [[nodiscard]] loop::ShardQueue<fabric::Port>& queue() noexcept {
            return queue_;
        }

        [[nodiscard]] Ip& ip() noexcept {
            return ip_;
        }

        [[nodiscard]] loop::TimerWheel& wheel() noexcept {
            return wheel_;
        }

        [[nodiscard]] Tcp& tcp() noexcept {
            return tcp_;
        }

        /// Transmits `frame` from the peer's port and runs IP and TCP over what the stack's port received, at `now`.
        void inject(std::span<const std::byte> frame, core::TimePoint now);

        /// The fabric's frames and loans due by `now`, a receive pass over what reached the stack's port (an empty one
        /// still stamps TCP), then the wheel: the first half of a tick with nothing injected.
        void step(core::TimePoint now);

        /// TCP's flush, then the queue's.
        void flush(core::TimePoint now);

        /// Every frame the peer's port received since the last call, oldest first, parsed and checked.
        [[nodiscard]] std::vector<Collected> collect();

    private:
        /// What the stack's port holds, through IP and TCP at `now`; there may be nothing.
        void receive(core::TimePoint now);

        HarnessConfig config_;
        fabric::Fabric fabric_;
        fabric::Port* peer_port_;
        fabric::Port* stack_port_;
        loop::ShardCounters counters_;
        loop::ShardQueue<fabric::Port> queue_;
        Ip ip_;
        loop::TimerWheel wheel_;
        Tcp tcp_;
        std::vector<Packet> burst_;
    };

}  // namespace aloe::scenario
