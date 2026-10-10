#pragma once

#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_test_device.hpp>

namespace aloe::testing {

    inline constexpr std::uint16_t tcp_listen_port   = 7;
    inline constexpr std::uint16_t tcp_peer_port     = 40000;
    inline constexpr std::uint32_t tcp_peer_isn      = 1000;
    inline constexpr std::uint16_t tcp_fabric_mss    = 1460;                  ///< MTU 1500 less both headers.
    inline constexpr std::uint32_t tcp_fabric_budget = 32U * tcp_fabric_mss;  ///< 46720: the default byte budget.

    /// The peer's outbound orientation towards the stack's listener.
    [[nodiscard]] inline frames::TcpSpec peer_spec(const std::uint16_t peer_port = tcp_peer_port) {
        return {.destination_mac  = stack_mac,
                .source_mac       = harness_mac,
                .source           = harness_ip,
                .destination      = stack_ip,
                .source_port      = peer_port,
                .destination_port = tcp_listen_port,
                .mss              = tcp_fabric_mss};
    }

    [[nodiscard]] inline bool is_flags(const frames::ParsedFrame& frame, const wire::TcpFlags flags) {
        return frame.tcp.has_value() && frame.tcp->flags == flags;
    }

    namespace detail {

        /// A fabric with the harness port and the stack port, constructed before the harness that uses them.
        struct FabricPair {
            explicit FabricPair(const fabric::EmulatedOffloads offloads,
                                const std::uint16_t stack_queues = 1,
                                const std::size_t pool_size      = 64)
                : harness{fabric.add_port(
                      {.mac = harness_mac, .pool_size = 256, .queue_depth = 4096, .offloads = offloads})},
                  port{fabric.add_port(
                      {.mac = stack_mac, .queues = stack_queues, .pool_size = pool_size, .offloads = offloads})} {
            }

            fabric::Fabric fabric;
            fabric::Port& harness;
            fabric::Port& port;
        };

        struct TestDeviceHolder {
            explicit TestDeviceHolder(fabric::Port& port) noexcept
                : device{port} {
            }

            TcpTestDevice device;
        };

    }  // namespace detail

    /**
     * @brief The brick over one queue of a device, a peer scripted by the test, and the helpers
     * to inject, tick and collect. Time is `now_`: the epoch plus what the test adds.
     *
     * `receive` injects through IP and TCP without flushing; `advance` moves the stamp, runs an
     * empty `process` and advances the wheel; `collect` flushes TCP and the queue and returns what
     * the harness port received. No fatal assertion lives in a helper that returns a value.
     */
    template <device::IsDevice Device>
    class TcpHarness {
    public:
        using Packet     = typename Device::Packet;
        using Ipv4       = net::Ipv4<Device>;
        using Tcp        = tcp::Stack<Ipv4>;
        using Connection = typename Tcp::ConnectionType;
        using Event      = typename Tcp::EventType;
        using TimePoint  = core::TimePoint;

        /// `ring_capacity` 1 makes the first refused device transmit refuse the send itself: the refusing fixture's
        /// choice.
        TcpHarness(fabric::Port& harness,
                   Device& device,
                   const tcp::TcpConfig& config     = {},
                   const net::Ipv4Config& ip_config = stack_config(),
                   const std::size_t ring_capacity  = 16)
            : harness_{&harness},
              queue_{device, 0, ring_capacity, counters_},
              ip_{queue_, ip_config},
              tcp_{std::make_unique<Tcp>(ip_, wheel_, config)} {
            ip_.learn(harness_ip, harness_mac, now_);
        }

    protected:
        fabric::Port* harness_;
        loop::ShardCounters counters_;
        loop::ShardQueue<Device> queue_;
        Ipv4 ip_;
        loop::TimerWheel wheel_{std::chrono::milliseconds{1}, TimePoint{}};
        std::unique_ptr<Tcp> tcp_;
        TimePoint now_{};
        std::vector<Packet> burst_ = std::vector<Packet>(64);
        std::size_t last_burst_    = 0;
        frames::TcpPeer peer_{peer_spec(), wire::TcpSequence{tcp_peer_isn}};

        /// Replaces the stack: setup only, before any connection exists.
        void configure_tcp(const tcp::TcpConfig& config) {
            tcp_.reset();
            tcp_ = std::make_unique<Tcp>(ip_, wheel_, config);
        }

        /// Sends `frame` from the harness, then runs IP and TCP over what the stack port received. No flush.
        void receive(const std::span<const std::byte> frame) {
            auto packet = harness_->allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(frames::fill(*packet, frame));
            std::array<Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_->transmit(0, out), 1U);
            process_pending();
        }

        /// One receive pass over whatever the stack port holds, which may be nothing.
        void process_pending() {
            last_burst_ = queue_.receive(burst_);
            ip_.process(std::span<Packet>{burst_}.first(last_burst_), now_);
            tcp_->process(ip_.received(wire::Ipv4Protocol::Tcp), now_);
        }

        /// Moves time, runs an empty process at the new stamp, then fires the timers due.
        void advance(const core::Duration by) {
            now_ += by;
            tcp_->process({}, now_);
            std::ignore = wheel_.advance(now_);
        }

        /// Flushes TCP and the queue, then returns every frame the harness received, oldest first.
        [[nodiscard]] std::vector<frames::ParsedFrame> collect() {
            tcp_->flush(now_);
            return collect_queued();
        }

        /// Flushes only the queue: what was already queued, without TCP's pending ACKs.
        [[nodiscard]] std::vector<frames::ParsedFrame> collect_queued() {
            std::ignore = queue_.flush();
            std::vector<frames::ParsedFrame> frames;
            std::array<Packet, 16> out;
            for (std::size_t count = harness_->receive(0, out); count > 0; count = harness_->receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    if (auto parsed = frames::parse_frame(frames::bytes_of(out[index]))) {
                        frames.push_back(std::move(*parsed));
                    }
                    out[index] = Packet{};
                }
            }
            return frames;
        }

        [[nodiscard]] std::optional<Event> poll() {
            return tcp_->poll_event();
        }

        /// A scripted passive open: SYN in, SYN-ACK out, ACK in, `Accepted` drained. Null on failure.
        [[nodiscard]] Connection* open_passive(frames::TcpPeer& peer) {
            if (!tcp_->listening(tcp_listen_port)) {
                std::ignore = tcp_->listen(tcp_listen_port);
            }
            receive(peer.syn());
            const auto frames = collect();
            if (frames.size() != 1 || !is_flags(frames[0], {wire::TcpFlag::Syn, wire::TcpFlag::Ack})) {
                ADD_FAILURE() << "expected one SYN-ACK, got " << frames.size() << " frames";
                return nullptr;
            }
            peer.see(frames[0]);
            receive(peer.ack());
            const auto event = poll();
            if (!event || !event->events.accepted()) {
                ADD_FAILURE() << "expected the Accepted event";
                return nullptr;
            }
            return event->connection;
        }

        /// A scripted active open: SYN out, SYN-ACK in, `Connected` drained, the deferred ACK flushed. Null on failure.
        [[nodiscard]] Connection* open_active(frames::TcpPeer& peer) {
            const auto connected = tcp_->connect({.address = harness_ip, .port = peer.spec().source_port}, now_);
            if (!connected) {
                ADD_FAILURE() << "connect failed: " << static_cast<int>(connected.error());
                return nullptr;
            }
            const auto frames = collect();
            if (frames.size() != 1 || !is_flags(frames[0], {wire::TcpFlag::Syn})) {
                ADD_FAILURE() << "expected one SYN, got " << frames.size() << " frames";
                return nullptr;
            }
            receive(peer.syn_ack(frames[0]));
            const auto event = poll();
            if (!event || !event->events.connected()) {
                ADD_FAILURE() << "expected the Connected event";
                return nullptr;
            }
            const auto acks = collect();
            if (acks.size() != 1 || !is_flags(acks[0], {wire::TcpFlag::Ack})) {
                ADD_FAILURE() << "expected the handshake ACK, got " << acks.size() << " frames";
                return nullptr;
            }
            peer.see(acks[0]);
            return *connected;
        }

        /// Every slot the last process was handed is empty afterwards.
        [[nodiscard]] bool burst_empty() const {
            return std::ranges::all_of(std::span<const Packet>{burst_}.first(last_burst_),
                                       [](const Packet& packet) { return packet.empty(); });
        }
    };

    /// The brick over a fabric port: the fixture most TCP tests use.
    class TcpFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads>,
                       protected detail::FabricPair,
                       protected TcpHarness<fabric::Port> {
    protected:
        TcpFixture()
            : FabricPair{GetParam()},
              TcpHarness{harness, port} {
        }

        [[nodiscard]] static bool offloads() {
            return GetParam() == fabric::EmulatedOffloads::Checksums;
        }
    };

    /// The brick over a device that can refuse: for refused sends and failed allocations.
    class TcpRefusingFixture : public ::testing::TestWithParam<fabric::EmulatedOffloads>,
                               protected detail::FabricPair,
                               protected detail::TestDeviceHolder,
                               protected TcpHarness<TcpTestDevice> {
    protected:
        TcpRefusingFixture()
            : FabricPair{GetParam()},
              TestDeviceHolder{port},
              TcpHarness{harness, device, {}, stack_config(), 1} {
        }

        /// From now on every send is refused: the device takes nothing, and an empty packet fills the one-slot ring
        /// so the very next transmit meets a full ring and a refusing device. The fabric drops the empty packet later.
        void refuse() {
            device.refuse_transmit = true;
            auto filler            = port.allocate(0);
            ASSERT_TRUE(filler.has_value());
            std::ignore = queue_.transmit(std::move(*filler));
        }

        void allow() {
            device.refuse_transmit = false;
        }

        [[nodiscard]] static bool offloads() {
            return GetParam() == fabric::EmulatedOffloads::Checksums;
        }
    };

}  // namespace aloe::testing
