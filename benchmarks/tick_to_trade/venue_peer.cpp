#include "venue_peer.hpp"

#include <cassert>
#include <format>
#include <tuple>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        [[nodiscard]] frames::TcpSpec peer_spec(const PeerConfig& config) {
            return {.destination_mac  = config.stack_mac,
                    .source_mac       = config.peer_mac,
                    .source           = config.peer_ip,
                    .destination      = config.stack_ip,
                    .source_port      = config.peer_port,
                    .destination_port = config.stack_port,
                    .mss              = config.mss};
        }

        constexpr wire::TcpSequence peer_initial_sequence{1000};

    }  // namespace

    VenuePeer::VenuePeer(ethdev::Port& port, const PeerConfig& config, std::span<std::uint64_t> starts)
        : port_{&port},
          config_{config},
          script_{peer_spec(config), peer_initial_sequence},
          starts_{starts} {
    }

    std::expected<void, PeerError> VenuePeer::open_active() {
        inject(script_.syn());
        return {};
    }

    std::expected<void, PeerError> VenuePeer::finish_active_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !parsed->tcp->flags.has(wire::TcpFlag::Syn) ||
            !parsed->tcp->flags.has(wire::TcpFlag::Ack)) {
            return std::unexpected{PeerError::Unexpected};
        }
        inject(script_.ack(*parsed));
        carrier_ = std::move(*packet);
        return {};
    }

    std::expected<void, PeerError> VenuePeer::answer_passive_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !parsed->tcp->flags.has(wire::TcpFlag::Syn) ||
            parsed->tcp->flags.has(wire::TcpFlag::Ack)) {
            return std::unexpected{PeerError::Unexpected};
        }
        carrier_ = std::move(*packet);  // the SYN's mbuf carries the SYN-ACK back: the stack frees it on its core
        if (prepare_carrier(script_.syn_ack(*parsed))) {
            send_carrier();
        }
        return {};
    }

    std::expected<void, PeerError> VenuePeer::finish_passive_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !is_pure_ack(*parsed)) {
            return std::unexpected{PeerError::Unexpected};
        }
        script_.see(*parsed);
        ++counters_.surplus_freed;
        return {};
    }

    void VenuePeer::send_tick(const std::uint64_t round, const std::size_t payload) {
        if (carrier_.empty()) {  // the floor has no handshake to seed the carrier: one mbuf of the peer's pool, once
            std::optional<Packet> fresh = port_->allocate(0);
            if (!fresh) {
                return;
            }
            carrier_ = std::move(*fresh);
        }
        tick_ = script_.data(tick_payload(payload, round));  // frees the last tick's frame, before the stamp
        if (!prepare_carrier(tick_)) {
            return;
        }
        starts_[round] = core::TscClock::get_ticks();  // the frame is in the mbuf: the window opens at the transmit
        send_carrier();
    }

    std::expected<void, PeerError> VenuePeer::await_order(const std::uint64_t round) {
        for (;;) {
            std::optional<Packet> packet = collect();
            if (!packet) {
                return std::unexpected{PeerError::Timeout};
            }
            const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
            if (!parsed || !parsed->tcp) {
                ++counters_.unexpected_frames;
                ++counters_.surplus_freed;
                continue;  // the packet frees as it goes out of scope
            }
            if (is_pure_ack(*parsed)) {
                ++counters_.pure_acks_seen;
                if (config_.strict) {
                    script_.see(*parsed);
                }
                ++counters_.surplus_freed;
                continue;
            }
            if (config_.strict) {
                if (frames::l4_checksum_residue(*parsed->ipv4, parsed->l4) != 0 ||
                    wire::internet_checksum(parsed->ipv4_header) != 0) {
                    return std::unexpected{PeerError::BadChecksum};
                }
                if (parsed->tcp->sequence != script_.rcv_nxt()) {
                    return std::unexpected{PeerError::WrongSequence};
                }
                script_.see(*parsed);
            }
            const std::span<const std::byte> payload = frames::tcp_payload(*parsed);
            if (payload.size() < round_prefix || read_round(payload) != round) {
                return std::unexpected{PeerError::WrongRound};
            }
            ++counters_.orders_seen;
            carrier_ = std::move(*packet);
            return {};
        }
    }

    void VenuePeer::send_bare_ack() {
        if (prepare_carrier(script_.ack())) {
            send_carrier();
        }
    }

    std::optional<VenuePeer::Packet> VenuePeer::collect() {
        const core::TimePoint deadline = core::Clock::now() + config_.deadline;
        for (;;) {
            while (next_ < pending_) {
                Packet taken = std::move(burst_[next_++]);
                if (next_ == pending_) {
                    next_ = pending_ = 0;
                }
                const std::span<const std::byte> data = taken.data();
                if (data.size() >= wire::EthernetHeader::size &&
                    wire::load_be16(data.subspan(12, 2)) == frames::ethertype_arp) {
                    if (const auto parsed = frames::parse_frame(data); parsed && parsed->arp) {
                        answer_arp(*parsed);
                    }
                    ++counters_.surplus_freed;
                    continue;
                }
                return taken;
            }
            pending_ = port_->receive(0, burst_);
            next_    = 0;
            if (pending_ == 0 && core::Clock::now() >= deadline) {
                return std::nullopt;
            }
        }
    }

    void VenuePeer::inject(const std::span<const std::byte> frame) {
        std::optional<Packet> packet = port_->allocate(0);
        if (!packet || !frames::fill(*packet, frame)) {
            return;  // the round that waits for the answer times out and names the peer
        }
        std::array<Packet, 1> out{std::move(*packet)};
        std::ignore = port_->transmit(0, out);
    }

    bool VenuePeer::prepare_carrier(const std::span<const std::byte> frame) {
        assert(!carrier_.empty() && "the carrier is the stack's last frame; the handshake seeds it");
        carrier_.trim_back(carrier_.size());
        return frames::fill(carrier_, frame);
    }

    void VenuePeer::send_carrier() noexcept {
        std::array<Packet, 1> out{std::move(carrier_)};
        if (port_->transmit(0, out) != 1) {
            carrier_ = std::move(out[0]);  // a refused transmit leaves the packet in the burst
        }
    }

    void VenuePeer::answer_arp(const frames::ParsedFrame& request) {
        if (request.arp->operation != wire::ArpOperation::Request || request.arp->target_ip != config_.peer_ip) {
            return;
        }
        inject(frames::arp_frame(
            frames::arp_reply(config_.peer_mac, config_.peer_ip, request.arp->sender_mac, request.arp->sender_ip),
            request.arp->sender_mac));
        ++counters_.arp_replies_sent;
    }

    bool VenuePeer::is_pure_ack(const frames::ParsedFrame& parsed) noexcept {
        const wire::TcpFlags flags = parsed.tcp->flags;
        return frames::tcp_payload(parsed).empty() && !flags.has(wire::TcpFlag::Syn) &&
               !flags.has(wire::TcpFlag::Fin) && !flags.has(wire::TcpFlag::Rst);
    }

    void drive_peer(VenuePeer& peer, RunState& state, const bool handshake) {
        const auto fail = [&state](const PeerError error, const std::uint64_t round) {
            state.fail(std::format("peer, round {}: {}", round, to_string(error)));
        };
        if (handshake) {
            const auto opened =
                state.config.round == Round::Tick
                    ? peer.open_active().and_then([&peer] { return peer.finish_active_open(); })
                    : peer.answer_passive_open().and_then([&peer] { return peer.finish_passive_open(); });
            if (!opened) {
                state.fail(std::format("peer, handshake: {}", to_string(opened.error())));
                return;
            }
        }
        const std::uint64_t gap_ticks = ticks_for(state.config.gap);
        std::uint64_t resume_at       = 0;
        for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
            if (state.config.round == Round::Tick) {
                while (core::TscClock::get_ticks() < resume_at) {}
                peer.send_tick(round, state.config.payload);
                if (const auto seen = peer.await_order(round); !seen) {
                    fail(seen.error(), round);
                    return;
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            } else {
                if (const auto seen = peer.await_order(round); !seen) {
                    fail(seen.error(), round);
                    return;
                }
                peer.send_bare_ack();
            }
        }
    }

}  // namespace aloe::benchmarks
