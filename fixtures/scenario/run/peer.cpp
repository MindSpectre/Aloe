#include "peer.hpp"

#include <aloe/frames>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <script.hpp>

namespace aloe::scenario {

    Peer::Peer(const wire::TcpSequence isn, const std::uint16_t peer_port, const std::uint16_t stack_port) noexcept
        : isn_{isn},
          peer_port_{peer_port},
          stack_port_{stack_port} {
    }

    wire::TcpSequence Peer::isn() const noexcept {
        return isn_;
    }

    std::optional<wire::TcpSequence> Peer::stack_isn() const noexcept {
        return stack_isn_;
    }

    std::uint16_t Peer::peer_port() const noexcept {
        return peer_port_;
    }

    std::uint16_t Peer::stack_port() const noexcept {
        return stack_port_;
    }

    std::optional<ObservedSegment> Peer::observe(const frames::ParsedFrame& frame) {
        if (!frame.tcp) {
            return std::nullopt;
        }
        const wire::TcpHeader& header = *frame.tcp;
        if (header.flags.has(wire::TcpFlag::Syn) && !stack_isn_) {
            stack_isn_  = header.sequence;
            stack_port_ = header.source_port;
        }
        ObservedSegment seen;
        seen.flags  = header.flags;
        seen.first  = stack_isn_ ? static_cast<std::uint32_t>(header.sequence - *stack_isn_) : header.sequence.value;
        seen.length = static_cast<std::uint32_t>(frames::tcp_payload(frame).size());
        seen.last   = seen.first + seen.length;
        if (header.flags.has(wire::TcpFlag::Ack)) {
            seen.ack = static_cast<std::uint32_t>(header.acknowledgement - isn_);
        }
        seen.window        = header.window;
        seen.urgent        = header.urgent_pointer;
        const auto options = frames::tcp_options_of(frame);
        seen.options.assign(options.begin(), options.end());
        if (const auto parsed = wire::TcpOptions::parse(seen.options); parsed && parsed->timestamp_value) {
            if (!timestamp_base_) {
                timestamp_base_ = *parsed->timestamp_value;
            }
            wire::store_be32(std::span{seen.options}.subspan(*parsed->timestamp_at + 2, 4),
                             *parsed->timestamp_value - *timestamp_base_);
        }
        return seen;
    }

    std::optional<std::uint32_t> Peer::stack_timestamp_base() const noexcept {
        return timestamp_base_;
    }

    std::uint32_t Peer::inbound_timestamp_base() const noexcept {
        return timestamp_base_.value_or(0U);
    }

    wire::TcpSequence Peer::sequence(const std::uint32_t relative) const noexcept {
        return isn_ + relative;
    }

    wire::TcpSequence Peer::acknowledgement(const std::uint32_t relative) const noexcept {
        return stack_isn_ ? *stack_isn_ + relative : wire::TcpSequence{relative};
    }

    wire::TcpSequence Peer::outbound_sack_base() const noexcept {
        return isn_;
    }

    wire::TcpSequence Peer::inbound_sack_base() const noexcept {
        return stack_isn_.value_or(wire::TcpSequence{});
    }

}  // namespace aloe::scenario
