#pragma once

#include <aloe/core>
#include <cstdint>

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
