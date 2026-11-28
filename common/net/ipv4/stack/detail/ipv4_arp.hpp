#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <arp_cache.hpp>
#include <arp_resolution.hpp>
#include <detail/ipv4_transmit.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>

namespace aloe::net::detail {

    /**
     * @brief ARP for one IPv4 brick: the cache, the requests it sends, the replies it gives and takes, and the
     * list of what the wire taught since the last `process`.
     *
     * Private to the brick, as ARP is to a socket. It reaches the queue alone: a request or a reply leaves through
     * `transmit_built`, and the brick's other parts ask it for a MAC or hand it an ARP frame. A frame never changes a
     * static entry, and with learning off no frame teaches anything and no request leaves; a request for our address
     * is answered in every case. With learning on a frame updates an entry the table holds and creates none
     * (`ArpCache::update`): only a lookup and the program's seed create one. With `report_unsolicited_replies` a reply
     * to us about an address the table does not hold is reported and not stored, for a sibling shard that asked; a
     * resolution a sibling reported is applied as a frame's mapping is.
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
                                    .request_interval = config.arp_request_interval,
                                    .learning         = config.arp_learning},
                     config.static_arp} {
            resolved_.reserve(config.burst_capacity);
        }

        /// An ARP frame for us: a request for our address is answered in its own packet, an entry the table holds
        /// takes the sender's mapping, and what the wire taught is reported.
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
                const bool known     = from_static_neighbour(*arp, now);
                if (arp->target_ip != config_.address) {
                    // Someone else's business; `process` releases it. RFC 826's merge rule: an entry we hold is
                    // updated, a new one is never created from a request not meant for us.
                    if (plausible && !known) {
                        update_sender(*arp, now);
                    }
                    return;
                }
                // A request for our address updates an entry we hold and creates none: where this departs from RFC 826,
                // whose merge adds the sender when we are the target, and from Linux, whose `arp_process` creates the
                // entry at once (`neigh_event_ns`). A frame that can create an entry can fill the table. A probe (RFC
                // 5227) is still answered, but teaches nothing.
                if (plausible && !known) {
                    update_sender(*arp, now);
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
            if (from_static_neighbour(*arp, now)) {
                return;
            }
            if (!cache_.contains(arp->sender_ip)) {
                // This shard never asked, and nothing on the segment fills its cache. A sibling shard may have: the
                // shards of a multi-queue device share our address and every ARP frame lands on queue 0, so with the
                // switch on a reply addressed to us is reported for the loop to forward, and still not stored. Linux's
                // `arp_process` drops it too unless `arp_accept` is set, and then creates the entry; Linux keeps one
                // table per device and has no sibling to tell.
                if (config_.arp_learning && config_.report_unsolicited_replies && arp->target_ip == config_.address &&
                    plausible_sender(*arp)) {
                    report(arp->sender_ip, arp->sender_mac);
                } else {
                    ++counters_.dropped_arp_unsolicited;
                }
                return;
            }
            if (plausible_sender(*arp)) {
                update_sender(*arp, now);
            }
        }

        /// The next hop's entry, with the request sent when the cache asks for one: its MAC, or nothing with a request
        /// on its way or with the learned table full, which is counted, and whether it is static.
        [[nodiscard]] ArpCache::Lookup lookup(const wire::Ipv4Address hop, const core::TimePoint now) noexcept {
            const ArpCache::Lookup found = cache_.lookup(hop, now);
            if (found.send_request) {
                request_arp(hop, found.mac);
            }
            if (found.full) {
                ++counters_.arp_table_full;
            }
            return found;
        }

        /// The next hop's MAC from the cache, or nothing with a request on its way.
        [[nodiscard]] std::optional<wire::MacAddress> resolve(const wire::Ipv4Address hop,
                                                              const core::TimePoint now) noexcept {
            return lookup(hop, now).mac;
        }

        /// ARP's half of the route question: whether a frame may go to the unicast `hop` at all. With learning on any
        /// neighbour may be resolved, with it off only a static one.
        [[nodiscard]] bool may_reach(const wire::Ipv4Address hop) const noexcept {
            return config_.arp_learning || cache_.is_static(hop);
        }

        /// The program's seed of the learned table: creates the entry when absent, and does nothing with learning off
        /// or for a static address; a full table refuses it, counted. Appends nothing to `resolved()`.
        void learn(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
            if (cache_.seed(address, mac, now) == ArpCache::Taught::Full) {
                ++counters_.arp_table_full;
            }
        }

        /// A resolution another shard reported, taken as a frame's mapping is (`ArpCache::update`): it fills, refreshes
        /// or changes the entry the table holds, extending its life only when it answers this brick's own request, and
        /// creates none. Reports and counts nothing: the shard whose queue received the frame did.
        void apply_resolution(const wire::Ipv4Address address,
                              const wire::MacAddress mac,
                              const core::TimePoint now) noexcept {
            std::ignore = cache_.update(address, mac, now);
        }

        /// The stamp a reachable or stale entry was last confirmed at (`ArpCache::confirmed`).
        [[nodiscard]] std::optional<core::TimePoint> confirmed(const wire::Ipv4Address hop) const noexcept {
            return cache_.confirmed(hop);
        }

        /// The cache's count of MAC changes (`ArpCache::generation`).
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return cache_.generation();
        }

        /// The mappings reported since `clear_resolved`: those frames gave entries the table holds, and with
        /// `report_unsolicited_replies` the replies to us about addresses it does not hold, which were not stored.
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

        /**
         * Whether the frame's sender address is static, so the frame teaches nothing and is reported nowhere. A sender
         * MAC other than the configured one is counted as a conflict and changes nothing else: the static entry stays
         * as configured, and a request for our address is still answered. Where this departs from Linux, whose
         * `arp_process` hands such a frame to `__neigh_update`, which refuses to change a NUD_PERMANENT entry and
         * counts nothing: the counter is what shows a router whose MAC changed (#23).
         */
        [[nodiscard]] bool from_static_neighbour(const wire::ArpPacket& arp, const core::TimePoint now) noexcept {
            if (!cache_.is_static(arp.sender_ip)) {
                return false;
            }
            if (cache_.lookup(arp.sender_ip, now).mac != arp.sender_mac) {  // a static lookup: no entry, no request
                ++counters_.dropped_arp_static_conflict;
            }
            return true;
        }

        /**
         * The frame's sender mapping, for the entry the table holds: filled, refreshed or changed. A frame creates no
         * entry, and extends an entry's life only when it answers the brick's own request. A mapping the entry took is
         * reported, the same MAC included and whether or not it answered this shard's request, so a sibling shard that
         * asked hears the answer. Anything else stored nothing and is not reported: an address the table does not
         * hold, a static one, or learning off.
         */
        void update_sender(const wire::ArpPacket& arp, const core::TimePoint now) noexcept {
            if (cache_.update(arp.sender_ip, arp.sender_mac, now) == ArpCache::Taught::Updated) {
                report(arp.sender_ip, arp.sender_mac);
            }
        }

        /// Appends the mapping to `resolved()` for the loop to forward, and counts it.
        void report(const wire::Ipv4Address address, const wire::MacAddress mac) noexcept {
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
