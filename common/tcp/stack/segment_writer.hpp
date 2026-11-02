#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/device>
#include <aloe/net>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <frame_template.hpp>
#include <tcp_counters.hpp>

namespace aloe::tcp::detail {

    /// The values of RFC 7323's option in one segment.
    struct TimestampOption {
        std::uint32_t value = 0;  ///< TSval: the sender's clock now.
        std::uint32_t echo  = 0;  ///< TSecr: TS.Recent.
    };

    /// The TCP part of a segment to frame: the header, whose data offset the writer sets, and its options.
    struct OutgoingSegment {
        wire::TcpHeader header{};
        std::optional<std::uint16_t> mss{};  ///< The MSS option, on a handshake segment.
        std::optional<TimestampOption> timestamps{};

        /// The TCP header and its options: one connection's segments of one kind keep one size, so a framed packet is
        /// rewritten in place.
        [[nodiscard]] std::size_t size() const noexcept {
            return wire::TcpHeader::size + (mss ? wire::TcpOptions::mss_size : 0) +
                   (timestamps ? wire::TcpOptions::timestamps_size : 0);
        }
    };

    /**
     * @brief Writes TCP segments into packets and hands them to the IP brick: the frame template, the header, the
     * option block, and the counting of what IP refused.
     *
     * It knows the wire format and nothing of TCP's state: the stack says what each segment carries. Every failure is
     * counted here, as `send_unresolved` when the next hop is unknown and `send_refused` otherwise.
     */
    template <typename Ip>
    class SegmentWriter {
    public:
        using Packet = Ip::Packet;

        SegmentWriter(Ip& ip, TcpCounters& counters) noexcept
            : ip_{&ip},
              counters_{&counters} {
        }

        /**
         * Places the frame's headers in front of the payload, prepended when the packet is not framed yet and over the
         * old ones when it is: the template's link and network part, rebuilt first when stale, the TCP header with a
         * zero checksum field, and the option block, the MSS first, then `nop, nop, TS`. `Ipv4::send_frame` finishes
         * the frame and lends the packet to the device; the packet stays with the caller, framed either way.
         */
        [[nodiscard]] std::expected<void, net::SendError> send(FrameTemplate& frame,
                                                               const wire::Ipv4Address destination,
                                                               Packet& packet,
                                                               const bool framed,
                                                               const OutgoingSegment& segment,
                                                               const core::TimePoint now) noexcept {
            assert(!packet.lent() && "a lent packet is not written");
            const std::size_t header_size = net::Prebuilt::size + segment.size();
            assert((!framed || packet.size() >= header_size) && "a framed packet holds its headers");
            const std::optional<std::span<std::byte>> headers =
                framed ? std::optional{packet.data().first(header_size)} : packet.prepend(header_size);
            assert(headers.has_value() && "a packet from allocate() has room for the headers");
            if (!headers) {
                ++counters_->send_refused;  // unreachable by construction; refused in a release build
                return std::unexpected{net::SendError::Refused};
            }
            if (const std::expected<void, net::SendError> built = refresh(frame, destination, now); !built) {
                count(built.error());
                return built;
            }
            std::ranges::copy(frame.headers.headers, headers->begin());
            write(headers->subspan(net::Prebuilt::size), segment);
            const std::expected<void, net::SendError> sent = ip_->send_frame(packet, request(destination), now);
            if (!sent) {
                count(sent.error());
            }
            return sent;
        }

        /// A segment with no payload in a fresh packet, through `Ipv4::send` and forgotten, since nothing of it is
        /// kept: a reset.
        [[nodiscard]] std::expected<void, net::SendError> send_detached(const wire::Ipv4Address destination,
                                                                        const OutgoingSegment& segment,
                                                                        const core::TimePoint now) noexcept {
            std::optional<Packet> packet = ip_->allocate();
            if (!packet) {
                ++counters_->send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            const std::optional<std::span<std::byte>> room = packet->append(segment.size());
            if (!room) {
                ++counters_->send_refused;
                return std::unexpected{net::SendError::Refused};
            }
            write(*room, segment);
            const std::expected<void, net::SendError> sent = ip_->send(std::move(*packet), request(destination), now);
            if (!sent) {
                count(sent.error());  // the packet is still in `packet` and goes back to the pool with it
            }
            return sent;
        }

        /**
         * A fresh packet holding a framed packet's first `header_size` bytes and `payload` bytes of its payload from
         * `skip` on, read from it and never written into it, so the original may be lent; nothing when the pool has
         * no packet. The original is taken by reference only because the packet concept offers `data()` on a non-const
         * packet.
         */
        [[nodiscard]] std::optional<Packet> copy(Packet& framed,
                                                 const std::size_t header_size,
                                                 const std::size_t skip,
                                                 const std::size_t payload) noexcept {
            std::optional<Packet> fresh = ip_->allocate();
            if (!fresh) {
                return std::nullopt;
            }
            const std::optional<std::span<std::byte>> bytes = fresh->append(header_size + payload);
            assert(bytes.has_value() && "a packet from allocate() holds the frame it copies");
            if (!bytes) {
                return std::nullopt;  // unreachable by construction
            }
            const std::span<const std::byte> frame = framed.data();
            std::ranges::copy(frame.first(header_size), bytes->begin());
            std::ranges::copy(frame.subspan(header_size + skip, payload), bytes->subspan(header_size).begin());
            return fresh;
        }

    private:
        [[nodiscard]] static net::SendRequest request(const wire::Ipv4Address destination) noexcept {
            return net::SendRequest{
                .destination = destination, .protocol = wire::Ipv4Protocol::Tcp, .checksum = device::L4Checksum::Tcp};
        }

        /// The TCP header and the option block, the data offset their size.
        static void write(const std::span<std::byte> tcp, const OutgoingSegment& segment) noexcept {
            wire::TcpHeader header = segment.header;
            header.data_offset     = static_cast<std::uint8_t>(segment.size());
            header.write(tcp);
            std::span<std::byte> options = tcp.subspan(wire::TcpHeader::size);
            if (segment.mss) {
                wire::TcpOptions::write_mss(options, *segment.mss);
                options = options.subspan(wire::TcpOptions::mss_size);
            }
            if (segment.timestamps) {
                wire::TcpOptions::write_timestamps(options, segment.timestamps->value, segment.timestamps->echo);
            }
        }

        /// Rebuilds the link and network headers when they went stale: never built, past their validity, or the IP
        /// brick's generation moved. On a failure the old headers stay stale.
        [[nodiscard]] std::expected<void, net::SendError>
        refresh(FrameTemplate& frame, const wire::Ipv4Address destination, const core::TimePoint now) noexcept {
            if (!frame.stale(now, ip_->generation())) {
                return {};
            }
            const std::expected<net::Prebuilt, net::SendError> built =
                ip_->prebuild(destination, wire::Ipv4Protocol::Tcp, now);
            if (!built) {
                return std::unexpected{built.error()};
            }
            frame.headers = *built;
            frame.built   = true;
            return {};
        }

        void count(const net::SendError error) noexcept {
            if (error == net::SendError::Unresolved) {
                ++counters_->send_unresolved;
            } else {
                ++counters_->send_refused;
            }
        }

        Ip* ip_                = nullptr;
        TcpCounters* counters_ = nullptr;
    };

}  // namespace aloe::tcp::detail
