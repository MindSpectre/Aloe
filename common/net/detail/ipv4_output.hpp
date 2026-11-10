#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

#include <detail/ipv4_arp.hpp>
#include <detail/ipv4_routing.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>
#include <prebuilt.hpp>

namespace aloe::net::detail {

    /**
     * @brief Everything that leaves the IPv4 brick as a datagram: `send` for a packet the caller lets go of,
     * `prebuild` and `send_frame` for a transport that keeps its packets, the checksums in one place and the
     * identification counter, and `frame_datagram`, the headers and checksums of a datagram to a known MAC, which
     * the echo reply is built with. Routes through `Routing`, resolves through `Arp`, transmits on the queue.
     */
    template <device::IsDevice Device>
    class Output {
    public:
        using Packet = Device::Packet;

        static constexpr std::size_t headers_size = wire::EthernetHeader::size + wire::Ipv4Header::size;

        Output(loop::ShardQueue<Device>& owner,
               Arp<Device>& arp,
               const Routing& routing,
               const Ipv4Config& config,
               Ipv4Counters& counters) noexcept
            : queue_{owner},
              arp_{arp},
              routing_{routing},
              config_{config},
              counters_{counters},
              identification_{static_cast<std::uint16_t>(owner.index() * identification_stride)} {
        }

        /// Routes, resolves, prepends both headers and transmits; the contract is on `Ipv4::send`.
        [[nodiscard]] std::expected<void, SendError>
        send(Packet&& packet, const SendRequest& request, const core::TimePoint now) noexcept {
            assert(packet.headroom() >= headers_size && "no room for the headers: use allocate()");
            assert((request.checksum == device::L4Checksum::None ||
                    packet.size() >= checksum_offset(request.checksum) + 2) &&
                   "a segment shorter than its checksum field");
            const std::expected<wire::MacAddress, SendError> mac = next_mac(request.destination, now);
            if (!mac) {
                return std::unexpected{mac.error()};
            }
            const wire::MacAddress hop_mac = *mac;
            const std::size_t segment_size = packet.size();
            if (segment_size > max_l4_size()) {
                ++counters_.send_oversized;
                return std::unexpected{SendError::Oversized};
            }
            if (!packet.prepend(headers_size)) {
                ++counters_.send_oversized;  // no headroom: not a packet from allocate()
                return std::unexpected{SendError::Oversized};
            }
            const std::span<std::byte> frame = packet.data();
            packet.set_tx(
                frame_datagram(frame, hop_mac, request.destination, request.protocol, segment_size, request.checksum));
            if (queue_.transmit(std::move(packet))) {
                ++counters_.datagrams_sent;
                return {};
            }
            // ShardQueue::transmit moves only on success, so the packet is still ours: hand it back as built.
            // NOLINTBEGIN(bugprone-use-after-move)
            if (request.checksum != device::L4Checksum::None) {
                wire::store_be16(frame.subspan(headers_size + checksum_offset(request.checksum), 2), 0);
            }
            packet.trim_front(headers_size);
            packet.set_tx(device::TxMetadata{});
            // NOLINTEND(bugprone-use-after-move)
            ++counters_.send_refused;
            return std::unexpected{SendError::Refused};
        }

        /// Routes, resolves and writes both headers into a kept packet's template; the contract is on `Ipv4::prebuild`.
        [[nodiscard]] std::expected<Prebuilt, SendError> prebuild(const wire::Ipv4Address destination,
                                                                  const wire::Ipv4Protocol protocol,
                                                                  const core::TimePoint now) noexcept {
            const std::expected<wire::MacAddress, SendError> mac = next_mac(destination, now);
            if (!mac) {
                return std::unexpected{mac.error()};
            }
            Prebuilt prebuilt{};
            wire::EthernetHeader{
                .destination = *mac, .source = queue_.mac(), .ethertype = std::to_underlying(wire::EtherType::Ipv4)}
                .write(std::span<std::byte>{prebuilt.headers}.first(wire::EthernetHeader::size));
            wire::Ipv4Header{.total_length   = 0,
                             .identification = 0,
                             .ttl            = config_.ttl,
                             .protocol       = protocol,
                             .source         = config_.address,
                             .destination    = destination}
                .write(std::span<std::byte>{prebuilt.headers}.subspan(wire::EthernetHeader::size));
            prebuilt.valid_until = core::TimePoint::max();
            if (!routing_.is_broadcast(destination) && !destination.is_multicast()) {
                const std::optional<core::TimePoint> confirmed = arp_.confirmed(*routing_.next_hop(destination));
                const bool fresh     = confirmed.has_value() && now - *confirmed < config_.arp_reachable;
                prebuilt.valid_until = fresh ? *confirmed + config_.arp_reachable : now + config_.arp_request_interval;
            }
            prebuilt.generation = arp_.generation();
            return prebuilt;
        }

        /// Finishes a prebuilt frame in place and transmits it; the contract is on `Ipv4::send_frame`.
        [[nodiscard]] std::expected<void, SendError>
        send_frame(Packet& packet, const SendRequest& request, [[maybe_unused]] const core::TimePoint now) noexcept {
            assert(!packet.empty() && !packet.lent() && "a frame that is empty or still on loan");
            assert((request.checksum == device::L4Checksum::None ||
                    packet.size() >= Prebuilt::size + checksum_offset(request.checksum) + 2) &&
                   "a segment shorter than its checksum field");
            const std::size_t frame_size = packet.size();
            if (frame_size > queue_.mtu() + wire::EthernetHeader::size) {
                ++counters_.send_oversized;
                return std::unexpected{SendError::Oversized};
            }
            const std::span<std::byte> frame = packet.data();
            const std::span<std::byte> ip    = frame.subspan(wire::EthernetHeader::size, wire::Ipv4Header::size);
            // The header as the Prebuilt wrote it, from what the caller holds rather than a parse of the frame: the
            // checksums need the addresses, the protocol and the total length only.
            const wire::Ipv4Header header{.total_length =
                                              static_cast<std::uint16_t>(frame_size - wire::EthernetHeader::size),
                                          .protocol    = request.protocol,
                                          .source      = config_.address,
                                          .destination = request.destination};
            wire::store_be16(ip.subspan(ipv4_total_length_offset, 2), header.total_length);
            wire::store_be16(ip.subspan(ipv4_identification_offset, 2), next_identification());
            wire::store_be16(ip.subspan(wire::ipv4_checksum_offset, 2), 0);
            assert(prebuilt_names(frame, header) && "the frame's IPv4 header is not the request's Prebuilt");
            packet.set_tx(finish_checksums(frame, header, request.checksum));
            Packet handle = packet.lend();
            if (queue_.transmit(std::move(handle))) {
                ++counters_.datagrams_sent;
                return {};
            }
            // ShardQueue::transmit moves only on success: drop the loan, then restore the frame as built.
            // NOLINTBEGIN(bugprone-use-after-move)
            handle = Packet{};
            wire::store_be16(ip.subspan(ipv4_total_length_offset, 2), 0);
            wire::store_be16(ip.subspan(ipv4_identification_offset, 2), 0);
            wire::store_be16(ip.subspan(wire::ipv4_checksum_offset, 2), 0);
            if (request.checksum != device::L4Checksum::None) {
                wire::store_be16(frame.subspan(Prebuilt::size + checksum_offset(request.checksum), 2), 0);
            }
            packet.set_tx(device::TxMetadata{});
            // NOLINTEND(bugprone-use-after-move)
            ++counters_.send_refused;
            return std::unexpected{SendError::Refused};
        }

        /// What one `send` may carry: the MTU less the IPv4 header.
        [[nodiscard]] std::uint16_t max_l4_size() const noexcept {
            return static_cast<std::uint16_t>(queue_.mtu() - wire::Ipv4Header::size);
        }

        /**
         * @brief Writes both headers of a datagram to a next hop whose MAC the caller knows, and completes the
         * checksums: what `send` does once the hop is resolved, and all the echo reply needs, since it goes back to
         * the MAC its request came from.
         *
         * `frame` starts with `headers_size` bytes for the headers, then `l4_size` bytes of segment or message; the
         * frame may be longer, as a padded one is. The datagram takes a fresh identification. Returns what the device
         * must finish (`finish_checksums`); the caller sets it on the packet and transmits.
         */
        [[nodiscard]] device::TxMetadata frame_datagram(const std::span<std::byte> frame,
                                                        const wire::MacAddress hop,
                                                        const wire::Ipv4Address destination,
                                                        const wire::Ipv4Protocol protocol,
                                                        const std::size_t l4_size,
                                                        const device::L4Checksum l4) noexcept {
            const wire::Ipv4Header header{.total_length = static_cast<std::uint16_t>(wire::Ipv4Header::size + l4_size),
                                          .identification = next_identification(),
                                          .ttl            = config_.ttl,
                                          .protocol       = protocol,
                                          .source         = config_.address,
                                          .destination    = destination};
            wire::EthernetHeader{
                .destination = hop, .source = queue_.mac(), .ethertype = std::to_underlying(wire::EtherType::Ipv4)}
                .write(frame.first(wire::EthernetHeader::size));
            header.write(frame.subspan(wire::EthernetHeader::size, wire::Ipv4Header::size));
            return finish_checksums(frame, header, l4);
        }

    private:
        static constexpr std::uint16_t identification_stride =
            4096;  ///< Per queue index, so shards do not collide early.
        static constexpr std::size_t ipv4_total_length_offset   = 2;
        static constexpr std::size_t ipv4_identification_offset = 4;
        static constexpr std::size_t tcp_checksum_offset        = 16;
        static constexpr std::size_t udp_checksum_offset        = 6;

        [[nodiscard]] static constexpr std::size_t checksum_offset(const device::L4Checksum l4) noexcept {
            return l4 == device::L4Checksum::Tcp ? tcp_checksum_offset : udp_checksum_offset;
        }

        /// Whether the frame's IPv4 header names what `header` does: the check behind `send_frame`'s trust in the
        /// caller's request, in an assertion only, so a release build never instantiates it.
        [[nodiscard]] static bool prebuilt_names(const std::span<const std::byte> frame,
                                                 const wire::Ipv4Header& header) noexcept {
            const std::optional<wire::Ipv4Header> parsed =
                wire::Ipv4Header::parse(frame.subspan(wire::EthernetHeader::size));
            return parsed.has_value() && parsed->source == header.source && parsed->destination == header.destination &&
                   parsed->protocol == header.protocol;
        }

        /**
         * Completes the checksums of a frame whose headers the brick wrote, and says what the device must finish.
         *
         * `frame` is Ethernet, the 20-byte IPv4 header with a zero checksum field, and the segment
         * `header.total_length` accounts for. When `l4` is not `None`, the segment's checksum field is zero and gets
         * the pseudo-header sum for the device to finish, or the full checksum in software, with UDP's zero written as
         * 0xffff. The IPv4 checksum is left for the device or written in software the same way.
         */
        [[nodiscard]] device::TxMetadata finish_checksums(const std::span<std::byte> frame,
                                                          const wire::Ipv4Header& header,
                                                          const device::L4Checksum l4) noexcept {
            device::TxMetadata tx{.l2_length          = static_cast<std::uint8_t>(wire::EthernetHeader::size),
                                  .l3_length          = static_cast<std::uint8_t>(wire::Ipv4Header::size),
                                  .fill_ipv4_checksum = false,
                                  .fill_l4_checksum   = device::L4Checksum::None};
            const std::span<std::byte> ip = frame.subspan(wire::EthernetHeader::size, wire::Ipv4Header::size);
            const std::span<std::byte> segment =
                frame.subspan(headers_size, header.total_length - wire::Ipv4Header::size);
            if (l4 != device::L4Checksum::None) {
                const std::span<std::byte> field = segment.subspan(checksum_offset(l4), 2);
                assert(wire::load_be16(field) == 0 && "the caller leaves the L4 checksum field zero");
                if (queue_.capabilities().tx_l4_checksum) {
                    wire::store_be16(field,
                                     wire::ipv4_pseudo_header_sum(header.source,
                                                                  header.destination,
                                                                  header.protocol,
                                                                  static_cast<std::uint16_t>(segment.size())));
                    tx.fill_l4_checksum = l4;
                } else {
                    std::uint16_t sum =
                        wire::ipv4_l4_checksum(header.source, header.destination, header.protocol, segment);
                    if (l4 == device::L4Checksum::Udp && sum == 0) {
                        sum = 0xffff;  // zero means "no checksum" in UDP
                    }
                    wire::store_be16(field, sum);
                }
            }
            if (queue_.capabilities().tx_ipv4_checksum) {
                tx.fill_ipv4_checksum = true;  // the field is already zero
            } else {
                wire::store_be16(ip.subspan(wire::ipv4_checksum_offset, 2), wire::ipv4_header_checksum(ip));
            }
            return tx;
        }

        /// The next IPv4 identification, one per datagram this queue sends.
        [[nodiscard]] std::uint16_t next_identification() noexcept {
            return identification_++;
        }

        /// The MAC a frame to `destination` goes to: a group MAC, or the routed hop's from the cache, counting the
        /// failures as `send` and `prebuild` report them.
        [[nodiscard]] std::expected<wire::MacAddress, SendError> next_mac(const wire::Ipv4Address destination,
                                                                          const core::TimePoint now) noexcept {
            if (routing_.is_broadcast(destination)) {
                return wire::MacAddress::broadcast();
            }
            if (destination.is_multicast()) {
                return destination.multicast_mac();
            }
            const std::optional<wire::Ipv4Address> hop = routing_.next_hop(destination);
            if (!hop) {
                ++counters_.send_no_route;
                return std::unexpected{SendError::NoRoute};
            }
            const std::optional<wire::MacAddress> mac = arp_.resolve(*hop, now);
            if (!mac) {
                ++counters_.send_unresolved;
                return std::unexpected{SendError::Unresolved};
            }
            return *mac;
        }

        loop::ShardQueue<Device>& queue_;
        Arp<Device>& arp_;
        const Routing& routing_;
        const Ipv4Config& config_;
        Ipv4Counters& counters_;
        std::uint16_t identification_;
    };

}  // namespace aloe::net::detail
