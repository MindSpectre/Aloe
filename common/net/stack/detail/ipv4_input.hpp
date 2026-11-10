#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <datagram.hpp>
#include <detail/ipv4_arp.hpp>
#include <detail/ipv4_output.hpp>
#include <detail/ipv4_routing.hpp>
#include <detail/ipv4_transmit.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>

namespace aloe::net::detail {

    /**
     * @brief What arrives at the IPv4 brick: the frame sorted by ethertype, ARP handed to `Arp`, IPv4 checked and
     * delivered into one list per transport, an echo request answered in its own packet, framed by `Output`. Every
     * packet handed in is moved into a list, transmitted as a reply, or left for the caller to release.
     */
    template <device::IsDevice Device>
    class Input {
    public:
        using Packet = Device::Packet;

        Input(loop::ShardQueue<Device>& owner,
              Arp<Device>& arp,
              Output<Device>& output,
              const Routing& routing,
              const Ipv4Config& config,
              Ipv4Counters& counters)
            : queue_{owner},
              arp_{arp},
              output_{output},
              routing_{routing},
              config_{config},
              counters_{counters} {
            tcp_.reserve(config.burst_capacity);
            udp_.reserve(config.burst_capacity);
        }

        /// Empties both transport lists; `Ipv4::process` calls it first.
        void clear() noexcept {
            tcp_.clear();
            udp_.clear();
        }

        /// One frame, counted and dispatched by ethertype.
        void receive(Packet& packet, const core::TimePoint now) noexcept {
            ++counters_.frames;
            const std::span<std::byte> frame                   = packet.data();
            const std::optional<wire::EthernetHeader> ethernet = wire::EthernetHeader::parse(frame);
            if (!ethernet) {
                ++counters_.dropped_short;
                return;
            }
            if (ethernet->destination != queue_.mac() && !ethernet->destination.is_broadcast()) {
                ++counters_.dropped_not_for_us;
                return;
            }
            if (ethernet->ethertype == std::to_underlying(wire::EtherType::Arp)) {
                arp_.receive(packet, now);
                return;
            }
            if (ethernet->ethertype == std::to_underlying(wire::EtherType::Ipv4)) {
                receive_ipv4(packet, *ethernet);
                return;
            }
            ++counters_.dropped_ethertype;
        }

        /// The datagrams of `protocol`, TCP or UDP, since the last `clear`.
        [[nodiscard]] std::span<Datagram<Packet>> received(const wire::Ipv4Protocol protocol) noexcept {
            assert(protocol == wire::Ipv4Protocol::Tcp || protocol == wire::Ipv4Protocol::Udp);
            return protocol == wire::Ipv4Protocol::Tcp ? std::span<Datagram<Packet>>{tcp_}
                                                       : std::span<Datagram<Packet>>{udp_};
        }

    private:
        void receive_ipv4(Packet& packet, const wire::EthernetHeader& ethernet) noexcept {
            const std::span<std::byte> frame             = packet.data();
            const std::span<std::byte> payload           = frame.subspan(wire::EthernetHeader::size);
            const std::optional<wire::Ipv4Header> header = wire::Ipv4Header::parse(payload);
            if (!header) {
                ++counters_.dropped_bad_header;
                return;
            }
            const device::RxMetadata rx = packet.rx();
            const bool bad_checksum     = rx.l3 == device::ChecksumVerdict::Bad ||
                                          (rx.l3 == device::ChecksumVerdict::Unknown &&
                                           wire::internet_checksum(payload.first(header->header_length)) != 0);
            if (bad_checksum) {
                ++counters_.dropped_bad_checksum;
                return;
            }
            if (header->source.is_multicast() || header->source.is_limited_broadcast() ||
                header->source.is_loopback() || header->source == config_.address) {
                ++counters_.dropped_martian;
                return;
            }
            if (!routing_.for_me(header->destination)) {
                ++counters_.dropped_not_for_us;
                return;
            }
            if (header->is_fragment()) {
                ++counters_.dropped_fragment;
                return;
            }
            if (header->protocol == wire::Ipv4Protocol::Tcp && header->destination != config_.address) {
                ++counters_.dropped_tcp_broadcast;  // RFC 1122 4.2.3.10: TCP never answers a broadcast
                return;
            }
            ++counters_.datagrams_received;
            switch (header->protocol) {
                case wire::Ipv4Protocol::Tcp:
                    deliver(tcp_, packet, *header, rx);
                    ++counters_.delivered_tcp;
                    return;
                case wire::Ipv4Protocol::Udp:
                    deliver(udp_, packet, *header, rx);
                    ++counters_.delivered_udp;
                    return;
                case wire::Ipv4Protocol::Icmp:
                    receive_icmp(packet, ethernet, *header);
                    return;
                default:
                    ++counters_.dropped_protocol;
                    return;
            }
        }

        void deliver(std::vector<Datagram<Packet>>& list,
                     Packet& packet,
                     const wire::Ipv4Header& header,
                     const device::RxMetadata& rx) noexcept {
            list.push_back(
                Datagram<Packet>{.packet      = std::move(packet),
                                 .source      = header.source,
                                 .destination = header.destination,
                                 .protocol    = header.protocol,
                                 .l3_offset   = static_cast<std::uint8_t>(wire::EthernetHeader::size),
                                 .l3_length   = header.header_length,
                                 .l4_length   = static_cast<std::uint16_t>(header.total_length - header.header_length),
                                 .l4_checksum = rx.l4});
        }

        /// An echo request to our unicast address becomes the reply, in the same packet, back to the frame's source
        /// MAC.
        void
        receive_icmp(Packet& packet, const wire::EthernetHeader& ethernet, const wire::Ipv4Header& header) noexcept {
            const std::span<std::byte> frame = packet.data();
            const std::size_t message_length = header.total_length - header.header_length;
            const std::span<std::byte> message =
                frame.subspan(wire::EthernetHeader::size + header.header_length, message_length);
            const std::optional<wire::IcmpHeader> icmp = wire::IcmpHeader::parse(message);
            if (!icmp) {
                ++counters_.dropped_bad_header;
                return;
            }
            if (wire::internet_checksum(message) != 0) {  // no card offloads ICMP: always software
                ++counters_.dropped_bad_checksum;
                return;
            }
            if (icmp->type != wire::IcmpType::EchoRequest || icmp->code != 0 || header.destination != config_.address) {
                ++counters_.dropped_icmp;  // other types, and echoes to a broadcast address, as the kernel does
                return;
            }
            // Aloe never sends options: drop them from the front. The message already sits after them and stays put.
            packet.trim_front(header.header_length - wire::Ipv4Header::size);
            const std::span<std::byte> reply         = packet.data();
            const std::span<std::byte> reply_message = reply.subspan(Output<Device>::headers_size, message_length);
            reply_message[0]                         = std::byte{std::to_underlying(wire::IcmpType::EchoReply)};
            wire::store_be16(reply_message.subspan(wire::IcmpHeader::checksum_offset, 2), 0);
            wire::store_be16(reply_message.subspan(wire::IcmpHeader::checksum_offset, 2),
                             wire::internet_checksum(reply_message));
            // The last hop is the next hop back, router or not: the reply goes to the MAC the request came from.
            packet.set_tx(output_.frame_datagram(reply,
                                                 ethernet.source,
                                                 header.source,
                                                 wire::Ipv4Protocol::Icmp,
                                                 message_length,
                                                 device::L4Checksum::None));
            transmit_built(queue_, std::move(packet), counters_.echo_replies_sent, counters_.transmit_refused);
        }

        loop::ShardQueue<Device>& queue_;
        Arp<Device>& arp_;
        Output<Device>& output_;
        const Routing& routing_;
        const Ipv4Config& config_;
        Ipv4Counters& counters_;
        std::vector<Datagram<Packet>> tcp_;
        std::vector<Datagram<Packet>> udp_;
    };

}  // namespace aloe::net::detail
