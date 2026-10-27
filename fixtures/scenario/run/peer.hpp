#pragma once

#include <aloe/frames>
#include <aloe/wire>
#include <cstdint>
#include <optional>

#include <script.hpp>

namespace aloe::scenario {

    /**
     * @brief The other end's sequence bookkeeping.
     *
     * Turns a frame the stack sent into an `ObservedSegment` in the script's relative numbers (relative to the stack's
     * ISN, which the first SYN fixes) and maps a script's inbound relative numbers to absolute ones.
     */
    class Peer {
    public:
        Peer(wire::TcpSequence isn, std::uint16_t peer_port, std::uint16_t stack_port) noexcept;

        [[nodiscard]] wire::TcpSequence isn() const noexcept;
        [[nodiscard]] std::optional<wire::TcpSequence> stack_isn() const noexcept;
        [[nodiscard]] std::uint16_t peer_port() const noexcept;

        /// The listen port, or the ephemeral port adopted from the stack's SYN.
        [[nodiscard]] std::uint16_t stack_port() const noexcept;

        /// A frame the stack sent: the first SYN fixes the stack's ISN and its port. Nothing for a frame that is not
        /// TCP.
        [[nodiscard]] std::optional<ObservedSegment> observe(const frames::ParsedFrame& frame);

        /// The absolute number for a script's inbound relative sequence: the peer's ISN plus it.
        [[nodiscard]] wire::TcpSequence sequence(std::uint32_t relative) const noexcept;

        /// The absolute number for a script's inbound relative acknowledgement: the stack's ISN plus it once known,
        /// else the number as written.
        [[nodiscard]] wire::TcpSequence acknowledgement(std::uint32_t relative) const noexcept;

        /// The base an outbound packet line's sack blocks are relative to: the peer's ISN, since the stack sacks the
        /// peer's data.
        [[nodiscard]] wire::TcpSequence outbound_sack_base() const noexcept;

        /// The base an inbound packet line's sack blocks are relative to: the stack's ISN, or zero until it is known.
        [[nodiscard]] wire::TcpSequence inbound_sack_base() const noexcept;

    private:
        wire::TcpSequence isn_;
        std::optional<wire::TcpSequence> stack_isn_{};
        std::uint16_t peer_port_;
        std::uint16_t stack_port_;
    };

}  // namespace aloe::scenario
