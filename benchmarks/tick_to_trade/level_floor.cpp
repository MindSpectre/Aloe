#include "level_floor.hpp"

#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Packet = ethdev::Packet;

        /// The order frame the floor transmits: stack to peer, PSH+ACK, fixed numbers; the peer is not strict here.
        [[nodiscard]] std::vector<std::byte> order_template(const PeerConfig& config) {
            const frames::TcpSpec spec{
                .destination_mac  = config.peer_mac,
                .source_mac       = config.stack_mac,
                .source           = config.stack_ip,
                .destination      = config.peer_ip,
                .source_port      = config.stack_port,
                .destination_port = config.peer_port,
                .sequence         = wire::TcpSequence{1},
                .acknowledgement  = wire::TcpSequence{1},
                .flags            = wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack},
                .mss              = std::nullopt
            };
            return frames::tcp_frame(spec, std::vector<std::byte>(order_size));
        }

        /// Allocates, fills the template, writes the order, arms, transmits. False when the device had no packet.
        [[nodiscard]] bool send_order(StampedDevice<ethdev::Port>& device,
                                      const std::span<const std::byte> frame,
                                      const std::uint64_t round) {
            std::optional<Packet> packet = device.allocate(0);
            if (!packet || !frames::fill(*packet, frame)) {
                return false;
            }
            write_order(packet->data().subspan(frame_header_size), round);
            device.arm(round);
            std::array<Packet, 1> out{std::move(*packet)};
            return device.transmit(0, out) == 1;
        }

        void run_floor_tick(RunState& state,
                            StampedDevice<ethdev::Port>& device,
                            const PeerConfig& peer_config,
                            VenuePeer* const inline_peer) {
            const std::vector<std::byte> frame = order_template(peer_config);
            std::vector<Packet> burst(ethdev::Port::max_burst);
            const std::size_t total = state.config.total();
            const auto step         = [&state, &device, &burst, &frame, total] {
                const std::size_t got = device.receive(0, burst);
                for (Packet& packet : std::span{burst}.first(got)) {
                    const std::span<const std::byte> payload =
                        std::span<const std::byte>{packet.data()}.subspan(frame_header_size);
                    const std::uint64_t round  = read_round(payload);
                    state.sink                ^= fold(payload);
                    packet                     = Packet{};  // the tick's mbuf, back to this core's pool
                    if (!send_order(device, frame, round)) {
                        state.fail("floor: no packet for the order");
                        return;
                    }
                    if (round + 1 == total) {
                        state.finish();
                    }
                }
            };
            if (inline_peer == nullptr) {
                while (!state.stop.load()) {
                    step();
                }
                return;
            }
            const std::uint64_t gap_ticks      = ticks_for(state.config.gap);
            const std::uint64_t deadline_ticks = ticks_for(peer_config.deadline);
            std::uint64_t resume_at            = 0;
            for (std::uint64_t round = 0; round < total && !state.stop.load(); ++round) {
                while (core::TscClock::get_ticks() < resume_at) {}
                const std::uint64_t give_up = core::TscClock::get_ticks() + deadline_ticks;  // read before the tick
                inline_peer->send_tick(round, state.config.payload);
                while (device.stamps_taken() <= round && !state.stop.load()) {
                    step();
                    if (device.stamps_taken() <= round && core::TscClock::get_ticks() > give_up) {
                        state.fail(std::format("floor, round {}: no tick within the deadline", round));
                    }
                }
                if (device.stamps_taken() <= round) {
                    break;  // the order never left, the run failed: there is nothing to await
                }
                if (const auto seen = inline_peer->await_order(round); !seen) {
                    state.fail(std::format("peer, round {}: {}", round, to_string(seen.error())));
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            }
        }

        void run_floor_gateway(RunState& state,
                               StampedDevice<ethdev::Port>& device,
                               const PeerConfig& peer_config,
                               VenuePeer* const inline_peer) {
            const std::vector<std::byte> frame = order_template(peer_config);
            std::vector<Packet> burst(ethdev::Port::max_burst);
            const std::uint64_t gap_ticks      = ticks_for(state.config.gap);
            const std::uint64_t deadline_ticks = ticks_for(peer_config.deadline);
            std::uint64_t resume_at            = 0;
            for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                while (core::TscClock::get_ticks() < resume_at) {}
                state.stamps.start[round] = core::TscClock::get_ticks();
                if (!send_order(device, frame, round)) {
                    state.fail("floor: no packet for the order");
                    return;
                }
                if (inline_peer != nullptr) {
                    if (const auto seen = inline_peer->await_order(round); !seen) {
                        state.fail(std::format("peer, round {}: {}", round, to_string(seen.error())));
                        return;
                    }
                    inline_peer->send_bare_ack();
                }
                const std::uint64_t give_up = core::TscClock::get_ticks() + deadline_ticks;
                std::size_t got             = 0;
                while (got == 0 && !state.stop.load()) {
                    got = device.receive(0, burst);
                    if (got == 0 && core::TscClock::get_ticks() > give_up) {
                        state.fail(std::format("floor, round {}: no acknowledgement within the deadline", round));
                        return;
                    }
                }
                for (Packet& packet : std::span{burst}.first(got)) {
                    packet = Packet{};  // the bare ACK rides in the order's own mbuf: back to this core's pool
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            }
            state.finish();
        }

    }  // namespace

    void run_floor(RunState& state,
                   StampedDevice<ethdev::Port>& device,
                   const PeerConfig& peer_config,
                   VenuePeer* const inline_peer) {
        if (state.config.round == Round::Tick) {
            run_floor_tick(state, device, peer_config, inline_peer);
        } else {
            run_floor_gateway(state, device, peer_config, inline_peer);
        }
    }

}  // namespace aloe::benchmarks
