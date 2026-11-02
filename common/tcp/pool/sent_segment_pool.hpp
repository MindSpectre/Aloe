#pragma once

#include <aloe/core>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <node_pool.hpp>
#include <packet.hpp>
#include <tcp_header.hpp>
#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief One entry of a connection's retransmission queue: a segment that occupies sequence space, SYN, SYN-ACK,
     * data or FIN, from its first transmission until an ACK covers it.
     *
     * The packet with the whole frame as it last left, and what a resend and a rate sample read.
     */
    template <device::IsPacket Packet>
    struct SentSegment {
        Packet packet{};            ///< The frame as it last left, headers included; lent while the device reads it.
        Packet copy{};              ///< The copy fallback's second packet while both are out; empty otherwise.
        wire::TcpSequence first{};  ///< The segment's first sequence number.
        std::uint32_t length  = 0;  ///< Sequence space: payload plus SYN and FIN.
        std::uint16_t payload = 0;  ///< Payload bytes in the packet.
        wire::TcpFlags flags{};     ///< As the segment carries them.
        core::TimePoint sent{};     ///< The last transmission; the epoch while never transmitted.
        core::TimePoint first_sent{};
        wire::TcpSequence copy_first{};  ///< The copy's first sequence number while a copy is out.
        std::uint16_t copy_payload = 0;  ///< The copy's payload bytes: the part not acknowledged when it left.
        std::uint8_t deferrals     = 0;  ///< Resends deferred since the last one that left.
        core::TimePoint deferred_at{};   ///< The last deferral: a later resend at the same stamp is the same one.
        std::uint8_t transmits          = 0;
        bool framed                     = false;  ///< The packet holds the frame headers in front of the payload.
        std::uint64_t delivered_at_send = 0;
        bool app_limited                = false;
        std::uint32_t next              = no_node;

        /// The SYN or the FIN: a control segment.
        [[nodiscard]] bool control() const noexcept {
            return flags.has(wire::TcpFlag::Syn) || flags.has(wire::TcpFlag::Fin);
        }

        /// A transmission left, from the packet or as a copy: stamped, the first one too, and counted, saturating.
        void stamp(const core::TimePoint now) noexcept {
            if (transmits == 0) {
                first_sent = now;
            }
            sent = now;
            if (transmits < std::numeric_limits<std::uint8_t>::max()) {
                ++transmits;
            }
        }

        /**
         * Settles a copy out once either packet is back. The original back: the copy is dropped. The copy back first:
         * it replaces the original, and the entry shrinks to the copy's range; the front it left out was counted as
         * delivered by the ACK that passed it. A dropped handle's buffer goes back now or when the device returns it.
         * Nothing with no copy out, or with both packets still lent.
         */
        void reconcile() noexcept {
            if (copy.empty()) {
                return;
            }
            if (!packet.lent()) {
                copy = Packet{};
                return;
            }
            if (copy.lent()) {
                return;
            }
            packet  = std::exchange(copy, Packet{});
            first   = copy_first;
            payload = copy_payload;
            length  = payload + (flags.has(wire::TcpFlag::Syn) ? 1U : 0U) + (flags.has(wire::TcpFlag::Fin) ? 1U : 0U);
        }

        /// The packet is back and `una` passed the first byte: the headers move forward over the acknowledged front,
        /// which is trimmed off, so a resend carries only what the peer lacks.
        void trim_to(const wire::TcpSequence una) noexcept {
            const auto acknowledged = static_cast<std::uint32_t>(una - first);
            assert(framed && !packet.lent() && acknowledged < payload && "only data is acknowledged in part");
            const std::span<std::byte> frame = packet.data();
            std::memmove(frame.subspan(acknowledged).data(), frame.data(), frame.size() - payload);
            packet.trim_front(acknowledged);
            first    = first + acknowledged;
            length  -= acknowledged;
            payload  = static_cast<std::uint16_t>(payload - acknowledged);
        }
    };

    /**
     * @brief The per-shard pool of retransmission-queue entries; a connection chains its sent segments on them, oldest
     * first.
     *
     * Every node may keep a packet of the device's pool, and a second one while the copy fallback has one out, so
     * that pool covers the receive descriptors, `receive_pool` held packets, `send_pool` kept packets and the copies
     * out, the transmit ring, what the card holds and the open preparations. A connection sets a node aside for its
     * FIN (`reserve`), so a close finds one whatever the others hold.
     */
    template <device::IsPacket Packet>
    using SentSegmentPool = NodePool<SentSegment<Packet>>;

}  // namespace aloe::tcp::detail
