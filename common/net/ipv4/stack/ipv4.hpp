#pragma once

#include <algorithm>
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
#include <stdexcept>
#include <utility>

#include <arp_cache.hpp>
#include <arp_resolution.hpp>
#include <detail/ipv4_arp.hpp>
#include <detail/ipv4_input.hpp>
#include <detail/ipv4_output.hpp>
#include <detail/ipv4_routing.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>
#include <ipv4_datagram.hpp>
#include <ipv4_prebuilt.hpp>

namespace aloe::net {

    /**
     * @brief IPv4 over one device queue, with the ARP and ICMP echo that serve it: the first protocol brick.
     *
     * One object per shard, between `loop::ShardQueue` and the transports. `process` consumes a burst and
     * leaves every slot empty; `received` and `resolved` are the lists the caller drains until the next
     * `process`; `send` prepends the headers and fills the checksums by the device's offload or in software.
     * ARP is private: the neighbours in `Ipv4Config::static_arp` resolve with no frame, and nothing else does
     * unless `Ipv4Config::arp_learning` is on; then `send` fails `Unresolved` while a request is out or the learned
     * table is full, `resolved` reports what frames taught the entries the table holds, `learn` and `resolve` seed
     * the cache, and no frame creates an entry. On a multi-queue device `Ipv4Config::report_unsolicited_replies` also
     * reports, without storing, a reply to us about an address the table does not hold, and `apply_resolution` takes
     * what a sibling shard reported as a frame's mapping, used only by a shard that asked. With learning off a next hop
     * outside the static table is `NoRoute`. Every member runs on the owning thread. Nothing here
     * is synchronised, blocks, throws after construction, logs, reads a clock, or allocates after
     * construction except packets.
     */
    template <device::IsDevice DeviceT>
    class Ipv4 {
    public:
        using Device = DeviceT;
        using Packet = Device::Packet;

        static constexpr std::size_t headers_size = detail::Output<Device>::headers_size;

        /// Validates and throws std::invalid_argument; transmits nothing.
        Ipv4(loop::ShardQueue<Device>& owner, const Ipv4Config& config)
            : queue_{owner},
              config_{validated(config)},
              routing_{config_.address, config_.prefix, config_.gateway},
              arp_{owner, config_, counters_},
              output_{owner, arp_, routing_, config_, counters_},
              input_{owner, arp_, output_, routing_, config_, counters_} {
        }

        Ipv4(const Ipv4&)            = delete;
        Ipv4& operator=(const Ipv4&) = delete;
        Ipv4(Ipv4&&)                 = delete;
        Ipv4& operator=(Ipv4&&)      = delete;
        ~Ipv4()                      = default;

        /**
         * @brief Consumes `burst`: ARP and echo answered, junk dropped and counted, datagrams for the
         * transports parsed into their lists. Clears the three lists first. Every slot is empty afterwards.
         */
        void process(std::span<Packet> burst, const core::TimePoint now) noexcept {
            assert(burst.size() <= config_.burst_capacity && "a burst larger than Ipv4Config::burst_capacity");
            input_.clear();
            arp_.clear_resolved();
            for (Packet& packet : burst) {
                if (packet.empty()) {
                    continue;
                }
                input_.receive(packet, now);
                packet = Packet{};  // whatever was neither moved out nor transmitted goes back to the pool
            }
        }

        /// The datagrams of `protocol`, TCP or UDP, from the last `process`, until the next one.
        [[nodiscard]] std::span<Ipv4Datagram<Packet>> received(const wire::Ipv4Protocol protocol) noexcept {
            return input_.received(protocol);
        }

        /// The mappings ARP frames gave entries the table holds in the last `process`, until the next one, and with
        /// `Ipv4Config::report_unsolicited_replies` the replies to us about addresses it does not hold, not stored.
        [[nodiscard]] std::span<const ArpResolution> resolved() const noexcept {
            return arp_.resolved();
        }

        /// A packet with nothing in it and room for both headers in front: append the segment, then `send`.
        [[nodiscard]] std::optional<Packet> allocate() noexcept {
            core::force_non_const(this);  // the brick's mutable view of the queue, though only the pointee is written
            return queue_.allocate();
        }

        /**
         * @brief Routes, resolves, prepends both headers, fills the checksums and queues the frame.
         *
         * `packet` holds the L4 segment and at least `headers_size` of headroom; `allocate()` gives one. With a
         * checksum requested, the segment holds the field: at least 8 bytes for UDP, 18 for TCP. The field is zero. The
         * packet is moved only on success; on any error it is handed back exactly as built, so a retry prepends again.
         */
        [[nodiscard]] std::expected<void, SendError>
        send(Packet&& packet, const SendRequest& request, const core::TimePoint now) noexcept {
            return output_.send(std::move(packet), request, now);
        }

        /**
         * @brief The headers of a frame to `destination`, to build once and reuse until `valid_until` or a new
         * generation.
         *
         * Routes and looks the hop up as `send` does, counting `send_no_route` and `send_unresolved` and sending the
         * ARP request when the cache asks. `valid_until` is the end of the entry's reachable time while it is fresh, a
         * request interval from `now` while it is stale, and never for a broadcast or multicast destination or a
         * static next hop.
         */
        [[nodiscard]] std::expected<Ipv4Prebuilt, SendError> prebuild(const wire::Ipv4Address destination,
                                                                      const wire::Ipv4Protocol protocol,
                                                                      const core::TimePoint now) noexcept {
            return output_.prebuild(destination, protocol, now);
        }

        /// Moves when a learned MAC changes or goes: an `Ipv4Prebuilt` of an older generation may name a stale next
        /// hop. The static table never moves it, and with learning off nothing does.
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return arp_.generation();
        }

        /**
         * @brief Finishes and transmits a frame the caller built from an `Ipv4Prebuilt`, and keeps the packet.
         *
         * `packet` holds a whole frame: the first `Ipv4Prebuilt::size` bytes from `prebuild`, then the segment, whose
         * checksum field is zero when one is requested. The brick writes the total length, a fresh identification
         * and the checksums, lends the packet to the device and transmits the loan, so `packet` stays with the
         * caller for a later resend once `lent()` is false. On `Refused` the frame is as the caller built it (the
         * per-datagram fields back to zero).
         */
        [[nodiscard]] std::expected<void, SendError>
        send_frame(Packet& packet, const SendRequest& request, const core::TimePoint now) noexcept {
            return output_.send_frame(packet, request, now);
        }

        /// The next hop's MAC from the cache, or nothing with a request on its way: how a program pre-resolves its
        /// router. With learning off, the static table's answer alone, and never a request.
        [[nodiscard]] std::optional<wire::MacAddress> resolve(const wire::Ipv4Address hop,
                                                              const core::TimePoint now) noexcept {
            return arp_.resolve(hop, now);
        }

        /**
         * @brief The program's seed of the learned table, and what it did (`ArpCache::seed`).
         *
         * `Created` for a new entry and `Updated` for one the table held, either way confirmed at `now` with `mac`.
         * Otherwise nothing is stored: `Full` when the table has no slot for a new address, counted in
         * `arp_table_full`; `Static` or `Conflict` for an address of the static table; and `Off` with learning off,
         * where a neighbour is named in `Ipv4Config::static_arp` instead. Appends nothing to `resolved()`. A
         * resolution another shard reported goes to `apply_resolution`, which creates nothing.
         */
        [[nodiscard]] ArpCache::Taught
        learn(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
            return arp_.learn(address, mac, now);
        }

        /// A resolution another shard reported: fills, refreshes or changes an entry this brick holds, and otherwise
        /// does nothing. Reports nothing in `resolved()`, so shards never echo each other. As for a frame's mapping, it
        /// extends the entry's life only when it answers a request this brick has outstanding (`ArpCache::update`).
        void apply_resolution(const wire::Ipv4Address address,
                              const wire::MacAddress mac,
                              const core::TimePoint now) noexcept {
            arp_.apply_resolution(address, mac, now);
        }

        /// The shard queue the brick was built over: a transport reads its index, steering, ring and device there.
        [[nodiscard]] loop::ShardQueue<Device>& queue() noexcept {
            return queue_;
        }

        [[nodiscard]] const loop::ShardQueue<Device>& queue() const noexcept {
            return queue_;
        }

        /**
         * @brief Where a unicast datagram to `destination` would be sent: the destination itself on the
         * subnet, the gateway off it, or nothing when there is none, or, with learning off, when that hop has no
         * static entry.
         *
         * A route question with no side effect: no ARP lookup, no request, no counter. `send` decides
         * the same way. Broadcast and multicast destinations are not routed and answer with themselves.
         */
        [[nodiscard]] std::optional<wire::Ipv4Address> next_hop(const wire::Ipv4Address destination) const noexcept {
            return output_.next_hop(destination);
        }

        [[nodiscard]] wire::Ipv4Address address() const noexcept {
            return config_.address;
        }

        [[nodiscard]] std::uint8_t prefix() const noexcept {
            return config_.prefix;
        }

        [[nodiscard]] std::optional<wire::Ipv4Address> gateway() const noexcept {
            return config_.gateway;
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return queue_.mtu();
        }

        /// What one `send` may carry: the MTU less the IPv4 header. TCP's MSS starts here.
        [[nodiscard]] std::uint16_t max_l4_size() const noexcept {
            return output_.max_l4_size();
        }

        [[nodiscard]] const Ipv4Counters& counters() const noexcept {
            return counters_;
        }

    private:
        /// An address a host may own on `subnet`: not zero, multicast, loopback or a broadcast, and not the subnet's
        /// network address where it has one.
        [[nodiscard]] static bool unicast_host(const wire::Ipv4Address address,
                                               const wire::Ipv4Subnet& subnet) noexcept {
            const bool reserved =
                subnet.has_broadcast() && (address == subnet.network() || address == subnet.broadcast());
            return !address.is_unspecified() && !address.is_multicast() && !address.is_loopback() &&
                   !address.is_limited_broadcast() && !reserved;
        }

        [[nodiscard]] static const Ipv4Config& validated(const Ipv4Config& config) {
            if (config.prefix > wire::Ipv4Subnet::max_prefix) {
                throw std::invalid_argument{"Ipv4Config::prefix must be at most 32"};
            }
            const wire::Ipv4Subnet subnet{config.address, config.prefix};
            if (!unicast_host(config.address, subnet)) {
                throw std::invalid_argument{"Ipv4Config::address must be a unicast host address on its subnet"};
            }
            if (config.ttl == 0) {
                throw std::invalid_argument{"Ipv4Config::ttl must be positive"};
            }
            if (config.burst_capacity == 0) {
                throw std::invalid_argument{"Ipv4Config::burst_capacity must be positive"};
            }
            if (config.gateway.has_value()) {
                if (!subnet.contains(*config.gateway)) {
                    throw std::invalid_argument{"Ipv4Config::gateway must be on the subnet"};
                }
                if (!unicast_host(*config.gateway, subnet) || *config.gateway == config.address) {
                    throw std::invalid_argument{"Ipv4Config::gateway must be another unicast host"};
                }
            }
            const std::span<const ArpResolution> table{config.static_arp};
            for (std::size_t index = 0; index < table.size(); ++index) {
                const auto& [address, mac] = table[index];
                if (!subnet.contains(address) || !unicast_host(address, subnet) || address == config.address) {
                    throw std::invalid_argument{
                        "Ipv4Config::static_arp: every address must be another unicast host on the subnet"};
                }
                if (mac == wire::MacAddress{} || mac.is_multicast()) {
                    throw std::invalid_argument{"Ipv4Config::static_arp: every MAC must be unicast and not zero"};
                }
                const std::span<const ArpResolution> earlier = table.first(index);
                if (std::ranges::find(earlier, address, &ArpResolution::address) != earlier.end()) {
                    throw std::invalid_argument{"Ipv4Config::static_arp lists an address twice"};
                }
            }
            if (!config.arp_learning && config.gateway.has_value() &&
                std::ranges::find(table, *config.gateway, &ArpResolution::address) == table.end()) {
                throw std::invalid_argument{"Ipv4Config::gateway needs a static_arp entry while arp_learning is off"};
            }
            return config;  // the ARP capacity and durations are ArpCache's to check, with learning on
        }

        loop::ShardQueue<Device>& queue_;
        Ipv4Config config_;
        detail::Routing routing_;
        Ipv4Counters counters_{};
        detail::Arp<Device> arp_;
        detail::Output<Device> output_;
        detail::Input<Device> input_;
    };

}  // namespace aloe::net
