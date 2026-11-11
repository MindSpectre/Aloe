#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arp_cache.hpp>
#include <datagram.hpp>
#include <detail/ipv4_transmit.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>

namespace aloe::net::detail {

    /**
     * @brief ARP for one IPv4 brick: the cache, the requests it sends, the replies it gives and takes, and the
     * list of what the wire taught since the last `process`.
     *
     * Private to the brick, as ARP is to a socket. It reaches the queue alone: a request or a reply leaves through
     * `transmit_built`, and the brick's other parts ask it for a MAC or hand it an ARP frame.
     */
    template <device::IsDevice Device>
    class Arp {
    public:
        using Packet = Device::Packet;

        Arp(loop::ShardQueue<Device>& owner, const Ipv4Config& config, Ipv4Counters& counters)
            : queue_{
                  owner
        },
              config_{config},
              counters_{counters},
              cache_{ArpCacheConfig{.capacity         = config.arp_capacity,
                                    .reachable        = config.arp_reachable,
                                    .expire           = config.arp_expire,
                                    .request_interval = config.arp_request_interval}} {
            resolved_.reserve(config.burst_capacity);
        }

        /// An ARP frame for us: a request is answered in its own packet, a reply is learned, and what the wire
        /// taught is reported.
        void receive(Packet& packet, const core::TimePoint now) noexcept {
            const std::span<std::byte> frame = packet.data();
            const std::optional<wire::ArpPacket> arp =
                wire::ArpPacket::parse(frame.subspan(wire::EthernetHeader::size));
            if (!arp) {
                ++counters_.dropped_arp_malformed;
                return;
            }
            if (arp->sender_ip == config_.address) {
                ++counters_.dropped_arp_conflict;
                return;
            }
            if (arp->operation == wire::ArpOperation::Request) {
                ++counters_.arp_requests_received;
                if (arp->sender_mac.is_multicast()) {
                    ++counters_.dropped_martian;  // the reply would go to a group, probe or not
                    return;
                }
                const bool plausible = plausible_sender(*arp);
                if (arp->target_ip != config_.address) {
                    // Someone else's business; `process` releases it. RFC 826's merge rule: an entry we hold is
                    // refreshed, a new one is never created from a request not meant for us.
                    if (plausible && cache_.contains(arp->sender_ip)) {
                        learn_from_wire(arp->sender_ip, arp->sender_mac, now);
                    }
                    return;
                }
                if (plausible) {  // a probe (RFC 5227) is still answered, but teaches nothing
                    learn_from_wire(arp->sender_ip, arp->sender_mac, now);
                }
                wire::EthernetHeader{.destination = arp->sender_mac,
                                     .source      = queue_.mac(),
                                     .ethertype   = std::to_underlying(wire::EtherType::Arp)}
                    .write(frame.first(wire::EthernetHeader::size));
                wire::ArpPacket{.operation  = wire::ArpOperation::Reply,
                                .sender_mac = queue_.mac(),
                                .sender_ip  = config_.address,
                                .target_mac = arp->sender_mac,
                                .target_ip  = arp->sender_ip}
                    .write(frame.subspan(wire::EthernetHeader::size));
                packet.set_tx(device::TxMetadata{});
                transmit_built(queue_, std::move(packet), counters_.arp_replies_sent, counters_.transmit_refused);
                return;
            }
            ++counters_.arp_replies_received;
            if (!cache_.contains(arp->sender_ip)) {
                // Never asked: nothing on the segment fills the cache, unless a sibling shard asked for our address.
                if (config_.accept_unsolicited_replies && arp->target_ip == config_.address && plausible_sender(*arp)) {
                    learn_from_wire(arp->sender_ip, arp->sender_mac, now);
                } else {
                    ++counters_.dropped_arp_unsolicited;
                }
                return;
            }
            if (plausible_sender(*arp)) {
                learn_from_wire(arp->sender_ip, arp->sender_mac, now);
            }
        }

        /// The next hop's MAC from the cache, or nothing with a request on its way.
        [[nodiscard]] std::optional<wire::MacAddress> resolve(const wire::Ipv4Address hop,
                                                              const core::TimePoint now) noexcept {
            const auto [mac, send_request] = cache_.lookup(hop, now);
            if (send_request) {
                request_arp(hop, mac);
            }
            return mac;
        }

        /// A static entry, or a resolution another shard learned. Appends nothing to `resolved()`.
        void learn(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
            cache_.learn(address, mac, now);
        }

        /// The stamp a reachable or stale entry was last confirmed at (`ArpCache::confirmed`).
        [[nodiscard]] std::optional<core::TimePoint> confirmed(const wire::Ipv4Address hop) const noexcept {
            return cache_.confirmed(hop);
        }

        /// The cache's count of MAC changes (`ArpCache::generation`).
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return cache_.generation();
        }

        /// The mappings learned from the wire since `clear_resolved`.
        [[nodiscard]] std::span<const ArpResolution> resolved() const noexcept {
            return resolved_;
        }

        /// Forgets the mappings reported since the last call; `process` calls it first.
        void clear_resolved() noexcept {
            resolved_.clear();
        }

    private:
        /// False for a sender that cannot be a host: zero, multicast, the limited broadcast, or a group MAC.
        [[nodiscard]] static bool plausible_sender(const wire::ArpPacket& arp) noexcept {
            return !arp.sender_ip.is_unspecified() && !arp.sender_ip.is_multicast() &&
                   !arp.sender_ip.is_limited_broadcast() && !arp.sender_mac.is_multicast();
        }

        void learn_from_wire(const wire::Ipv4Address address,
                             const wire::MacAddress mac,
                             const core::TimePoint now) noexcept {
            cache_.learn(address, mac, now);
            resolved_.push_back(ArpResolution{.address = address, .mac = mac});
            ++counters_.resolutions;
        }

        /// A request for `target`: broadcast for an unresolved entry, unicast to `known` for a stale one.
        void request_arp(const wire::Ipv4Address target, const std::optional<wire::MacAddress> known) noexcept {
            std::optional<Packet> packet = queue_.allocate();
            if (!packet) {
                ++counters_.transmit_refused;
                return;
            }
            const std::optional<std::span<std::byte>> room =
                packet->append(wire::EthernetHeader::size + wire::ArpPacket::size);
            if (!room) {
                ++counters_.transmit_refused;
                return;
            }
            wire::EthernetHeader{.destination = known.value_or(wire::MacAddress::broadcast()),
                                 .source      = queue_.mac(),
                                 .ethertype   = std::to_underlying(wire::EtherType::Arp)}
                .write(room->first(wire::EthernetHeader::size));
            wire::ArpPacket{.operation  = wire::ArpOperation::Request,
                            .sender_mac = queue_.mac(),
                            .sender_ip  = config_.address,
                            .target_mac = {},
                            .target_ip  = target}
                .write(room->subspan(wire::EthernetHeader::size));
            packet->set_tx(device::TxMetadata{});
            transmit_built(queue_, std::move(*packet), counters_.arp_requests_sent, counters_.transmit_refused);
        }

        loop::ShardQueue<Device>& queue_;
        const Ipv4Config& config_;
        Ipv4Counters& counters_;
        ArpCache cache_;
        std::vector<ArpResolution> resolved_;
    };

}  // namespace aloe::net::detail
