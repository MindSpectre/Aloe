#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/loop>
#include <aloe/wire>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <arp_cache.hpp>
#include <datagram.hpp>
#include <detail/ipv4_arp.hpp>
#include <detail/ipv4_routing.hpp>
#include <detail/ipv4_transmit.hpp>
#include <ipv4_config.hpp>
#include <ipv4_counters.hpp>
#include <prebuilt.hpp>

namespace aloe::net {

    /**
     * @brief IPv4 over one device queue, with the ARP and ICMP echo that serve it: the first protocol brick.
     *
     * One object per shard, between `loop::ShardQueue` and the transports. `process` consumes a burst and
     * leaves every slot empty; `received` and `resolved` are the lists the caller drains until the next
     * `process`; `send` prepends the headers and fills the checksums by the device's offload or in software.
     * ARP is private: `send` fails `Unresolved` while a request is out, `resolved` reports what was
     * learned, `learn` and `resolve` seed the cache. Every member runs on the owning thread. Nothing here
     * is synchronised, blocks, throws after construction, logs, reads a clock, or allocates after
     * construction except packets.
     */
    template <device::IsDevice DeviceT>
    class Ipv4 {
    public:
        using Device = DeviceT;
        using Packet = Device::Packet;

        static constexpr std::size_t headers_size = wire::EthernetHeader::size + wire::Ipv4Header::size;

        /// Validates and throws std::invalid_argument; transmits nothing.
        Ipv4(loop::ShardQueue<Device>& owner, const Ipv4Config& config)
            : queue_{owner},
              config_{validated(config)},
              routing_{config_.address, config_.prefix, config_.gateway},
              arp_{owner, config_, counters_},
              identification_{static_cast<std::uint16_t>(owner.index() * identification_stride)} {
            tcp_.reserve(config.burst_capacity);
            udp_.reserve(config.burst_capacity);
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
            tcp_.clear();
            udp_.clear();
            arp_.clear_resolved();
            for (Packet& packet : burst) {
                if (packet.empty()) {
                    continue;
                }
                ++counters_.frames;
                receive(packet, now);
                packet = Packet{};  // whatever was neither moved out nor transmitted goes back to the pool
            }
        }

        /// The datagrams of `protocol`, TCP or UDP, from the last `process`, until the next one.
        [[nodiscard]] std::span<Datagram<Packet>> received(const wire::Ipv4Protocol protocol) noexcept {
            assert(protocol == wire::Ipv4Protocol::Tcp || protocol == wire::Ipv4Protocol::Udp);
            return protocol == wire::Ipv4Protocol::Tcp ? std::span<Datagram<Packet>>{tcp_}
                                                       : std::span<Datagram<Packet>>{udp_};
        }

        /// The mappings learned from ARP frames in the last `process`, until the next one.
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
            const wire::Ipv4Header header{.total_length =
                                              static_cast<std::uint16_t>(wire::Ipv4Header::size + segment_size),
                                          .identification = next_identification(),
                                          .ttl            = config_.ttl,
                                          .protocol       = request.protocol,
                                          .source         = config_.address,
                                          .destination    = request.destination};
            wire::EthernetHeader{
                .destination = hop_mac, .source = queue_.mac(), .ethertype = std::to_underlying(wire::EtherType::Ipv4)}
                .write(frame.first(wire::EthernetHeader::size));
            header.write(frame.subspan(wire::EthernetHeader::size, wire::Ipv4Header::size));
            packet.set_tx(finish_checksums(frame, header, request.checksum));
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

        /**
         * @brief The headers of a frame to `destination`, to build once and reuse until `valid_until` or a new
         * generation.
         *
         * Routes and looks the hop up as `send` does, counting `send_no_route` and `send_unresolved` and sending the
         * ARP request when the cache asks. `valid_until` is the end of the entry's reachable time while it is fresh, a
         * request interval from `now` while it is stale, and never for a broadcast or multicast destination.
         */
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
                const std::optional<core::TimePoint> confirmed = arp_.confirmed(*next_hop(destination));
                const bool fresh     = confirmed.has_value() && now - *confirmed < config_.arp_reachable;
                prebuilt.valid_until = fresh ? *confirmed + config_.arp_reachable : now + config_.arp_request_interval;
            }
            prebuilt.generation = generation();
            return prebuilt;
        }

        /// Moves when a cached MAC changes or goes: a `Prebuilt` of an older generation may name a stale next hop.
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return arp_.generation();
        }

        /**
         * @brief Finishes and transmits a frame the caller built from a `Prebuilt`, and keeps the packet.
         *
         * `packet` holds a whole frame: the first `Prebuilt::size` bytes from `prebuild`, then the segment, whose
         * checksum field is zero when one is requested. The brick writes the total length, a fresh identification
         * and the checksums, lends the packet to the device and transmits the loan, so `packet` stays with the
         * caller for a later resend once `lent()` is false. On `Refused` the frame is as the caller built it (the
         * per-datagram fields back to zero).
         */
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

        /// Whether the frame's IPv4 header names what `header` does: the check behind `send_frame`'s trust in the
        /// caller's request, in an assertion only, so a release build never instantiates it.
        [[nodiscard]] static bool prebuilt_names(const std::span<const std::byte> frame,
                                                 const wire::Ipv4Header& header) noexcept {
            const std::optional<wire::Ipv4Header> parsed =
                wire::Ipv4Header::parse(frame.subspan(wire::EthernetHeader::size));
            return parsed.has_value() && parsed->source == header.source && parsed->destination == header.destination &&
                   parsed->protocol == header.protocol;
        }

        /// The next hop's MAC from the cache, or nothing with a request on its way: how a program pre-resolves its
        /// router.
        [[nodiscard]] std::optional<wire::MacAddress> resolve(const wire::Ipv4Address hop,
                                                              const core::TimePoint now) noexcept {
            return arp_.resolve(hop, now);
        }

        /// A static entry, or a resolution another shard learned. Appends nothing to `resolved()`.
        void learn(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
            arp_.learn(address, mac, now);
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
         * subnet, the gateway off it, or nothing when there is none.
         *
         * A route question with no side effect: no ARP lookup, no request, no counter. `send` decides
         * the same way. Broadcast and multicast destinations are not routed and answer with themselves.
         */
        [[nodiscard]] std::optional<wire::Ipv4Address> next_hop(const wire::Ipv4Address destination) const noexcept {
            return routing_.next_hop(destination);
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
            return static_cast<std::uint16_t>(queue_.mtu() - wire::Ipv4Header::size);
        }

        [[nodiscard]] const Ipv4Counters& counters() const noexcept {
            return counters_;
        }

    private:
        static constexpr std::uint16_t identification_stride =
            4096;  ///< Per queue index, so shards do not collide early.

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
            return config;  // the ARP capacity and durations are ArpCache's to check
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
            const std::optional<wire::Ipv4Address> hop = next_hop(destination);
            if (!hop) {
                ++counters_.send_no_route;
                return std::unexpected{SendError::NoRoute};
            }
            const std::optional<wire::MacAddress> mac = resolve(*hop, now);
            if (!mac) {
                ++counters_.send_unresolved;
                return std::unexpected{SendError::Unresolved};
            }
            return *mac;
        }

        [[nodiscard]] std::uint16_t next_identification() noexcept {
            return identification_++;
        }

        void receive(Packet& packet, const core::TimePoint now) noexcept {
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

        static constexpr std::size_t ipv4_total_length_offset   = 2;
        static constexpr std::size_t ipv4_identification_offset = 4;
        static constexpr std::size_t tcp_checksum_offset        = 16;
        static constexpr std::size_t udp_checksum_offset        = 6;

        [[nodiscard]] static constexpr std::size_t checksum_offset(const device::L4Checksum l4) noexcept {
            return l4 == device::L4Checksum::Tcp ? tcp_checksum_offset : udp_checksum_offset;
        }

        /**
         * @brief Completes the checksums of a frame whose headers the brick wrote, and says what the device must
         * finish.
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
            const std::span<std::byte> reply = packet.data();
            const wire::Ipv4Header reply_header{.total_length =
                                                    static_cast<std::uint16_t>(wire::Ipv4Header::size + message_length),
                                                .identification = next_identification(),
                                                .ttl            = config_.ttl,
                                                .protocol       = wire::Ipv4Protocol::Icmp,
                                                .source         = config_.address,
                                                .destination    = header.source};
            wire::EthernetHeader{.destination = ethernet.source,  // the last hop is the next hop back, router or not
                                 .source      = queue_.mac(),
                                 .ethertype   = std::to_underlying(wire::EtherType::Ipv4)}
                .write(reply.first(wire::EthernetHeader::size));
            reply_header.write(reply.subspan(wire::EthernetHeader::size, wire::Ipv4Header::size));
            const std::span<std::byte> reply_message = reply.subspan(headers_size, message_length);
            reply_message[0]                         = std::byte{std::to_underlying(wire::IcmpType::EchoReply)};
            wire::store_be16(reply_message.subspan(wire::IcmpHeader::checksum_offset, 2), 0);
            wire::store_be16(reply_message.subspan(wire::IcmpHeader::checksum_offset, 2),
                             wire::internet_checksum(reply_message));
            packet.set_tx(finish_checksums(reply, reply_header, device::L4Checksum::None));
            detail::transmit_built(queue_, std::move(packet), counters_.echo_replies_sent, counters_.transmit_refused);
        }

        loop::ShardQueue<Device>& queue_;
        Ipv4Config config_;
        detail::Routing routing_;
        Ipv4Counters counters_{};
        detail::Arp<Device> arp_;
        std::uint16_t identification_;
        std::vector<Datagram<Packet>> tcp_;
        std::vector<Datagram<Packet>> udp_;
    };

}  // namespace aloe::net
