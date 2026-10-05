#pragma once

#include <aloe/utils>
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

#include <address.hpp>
#include <arp.hpp>
#include <arp_cache.hpp>
#include <bytes.hpp>
#include <checksum.hpp>
#include <datagram.hpp>
#include <device.hpp>
#include <ethernet.hpp>
#include <icmp.hpp>
#include <ipv4_header.hpp>
#include <packet.hpp>
#include <protocol.hpp>
#include <shard_queue.hpp>

namespace aloe::net {

    struct Ipv4Config {
        device::Ipv4Address address{};
        std::uint8_t prefix                        = 24;
        std::optional<device::Ipv4Address> gateway = std::nullopt;  ///< Absent: an off-subnet destination is NoRoute.
        std::uint8_t ttl                           = 64;
        std::size_t burst_capacity                 = 64;   ///< The most packets one `process` takes; ethdev's burst.
        std::size_t arp_capacity                   = 256;  ///< A power of two of at least 8.
        std::chrono::nanoseconds arp_reachable     = std::chrono::seconds{60};
        std::chrono::nanoseconds arp_expire        = std::chrono::seconds{120};
        std::chrono::nanoseconds arp_request_interval = std::chrono::seconds{1};
    };

    struct SendRequest {
        device::Ipv4Address destination{};
        device::Ipv4Protocol protocol = device::Ipv4Protocol::Udp;
        device::L4Checksum checksum   = device::L4Checksum::None;  ///< Fill the L4 checksum field the caller left zero.
    };

    enum class SendError : std::uint8_t {
        NoRoute,     ///< Off the subnet with no gateway configured.
        Unresolved,  ///< The next hop's MAC is unknown; a request is on its way, retry later.
        Oversized,   ///< The segment does not fit the MTU.
        Refused,     ///< The ring and the device are both full.
    };

    /// Per-brick counters, all monotonic, read on the owning thread or after it has stopped.
    struct Ipv4Counters {
        std::uint64_t frames                  = 0;  ///< Handed to `process`.
        std::uint64_t arp_requests_received   = 0;
        std::uint64_t arp_replies_received    = 0;
        std::uint64_t datagrams_received      = 0;  ///< Valid IPv4 for us, whatever the protocol.
        std::uint64_t delivered_tcp           = 0;
        std::uint64_t delivered_udp           = 0;
        std::uint64_t resolutions             = 0;  ///< Mappings learned from ARP frames.
        std::uint64_t arp_requests_sent       = 0;
        std::uint64_t arp_replies_sent        = 0;
        std::uint64_t echo_replies_sent       = 0;
        std::uint64_t datagrams_sent          = 0;
        std::uint64_t transmit_refused        = 0;  ///< A frame the brick itself built that the queue refused.
        std::uint64_t dropped_short           = 0;
        std::uint64_t dropped_not_for_us      = 0;
        std::uint64_t dropped_ethertype       = 0;
        std::uint64_t dropped_arp_malformed   = 0;
        std::uint64_t dropped_arp_conflict    = 0;
        std::uint64_t dropped_arp_unsolicited = 0;
        std::uint64_t dropped_bad_header      = 0;
        std::uint64_t dropped_bad_checksum    = 0;
        std::uint64_t dropped_fragment        = 0;
        std::uint64_t dropped_protocol        = 0;
        std::uint64_t dropped_icmp            = 0;
        std::uint64_t dropped_martian =
            0;  ///< A source address no host may send from: multicast, limited broadcast, loopback.
        std::uint64_t send_no_route   = 0;
        std::uint64_t send_unresolved = 0;
        std::uint64_t send_oversized  = 0;
        std::uint64_t send_refused    = 0;

        friend constexpr bool operator==(const Ipv4Counters&, const Ipv4Counters&) noexcept = default;
    };

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
    template <device::IsDevice Device>
    class Ipv4 {
    public:
        using Packet    = typename Device::Packet;
        using TimePoint = std::chrono::steady_clock::time_point;  ///< The loop's clock; a stamp the caller passes.

        static constexpr std::size_t headers_size = ethernet_header_size + ipv4_header_size;
        static constexpr device::Ipv4Address limited_broadcast{255, 255, 255, 255};

        /// Validates and throws std::invalid_argument; transmits nothing.
        Ipv4(loop::ShardQueue<Device>& owner, const Ipv4Config& config)
            : queue_{
                  &owner
        },
              config_{validated(config)},
              mask_{mask_of(config.prefix)},
              arp_{ArpCacheConfig{.capacity         = config.arp_capacity,
                                  .reachable        = config.arp_reachable,
                                  .expire           = config.arp_expire,
                                  .request_interval = config.arp_request_interval}},
              identification_{static_cast<std::uint16_t>(owner.index() * identification_stride)} {
            tcp_.reserve(config.burst_capacity);
            udp_.reserve(config.burst_capacity);
            resolved_.reserve(config.burst_capacity);
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
        void process(std::span<Packet> burst, const TimePoint now) noexcept {
            assert(burst.size() <= config_.burst_capacity && "a burst larger than Ipv4Config::burst_capacity");
            tcp_.clear();
            udp_.clear();
            resolved_.clear();
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
        [[nodiscard]] std::span<Datagram<Packet>> received(const device::Ipv4Protocol protocol) noexcept {
            assert(protocol == device::Ipv4Protocol::Tcp || protocol == device::Ipv4Protocol::Udp);
            return protocol == device::Ipv4Protocol::Tcp ? std::span<Datagram<Packet>>{tcp_}
                                                         : std::span<Datagram<Packet>>{udp_};
        }

        /// The mappings learned from ARP frames in the last `process`, until the next one.
        [[nodiscard]] std::span<const ArpResolution> resolved() const noexcept {
            return resolved_;
        }

        /// A packet with nothing in it and room for both headers in front: append the segment, then `send`.
        [[nodiscard]] std::optional<Packet> allocate() noexcept {
            utils::force_non_const(this);  // the brick's mutable view of the queue, though only the pointee is written
            return queue_->allocate();
        }

        /**
         * @brief Routes, resolves, prepends both headers, fills the checksums and queues the frame.
         *
         * `packet` holds the L4 segment and at least `headers_size` of headroom; `allocate()` gives one. With a
         * checksum requested, the segment holds the field: at least 8 bytes for UDP, 18 for TCP. The field is zero. The
         * packet is moved only on success; on any error it is handed back exactly as built, so a retry prepends again.
         */
        [[nodiscard]] std::expected<void, SendError>
        send(Packet&& packet, const SendRequest& request, const TimePoint now) noexcept {
            assert(packet.headroom() >= headers_size && "no room for the headers: use allocate()");
            assert((request.checksum == device::L4Checksum::None ||
                    packet.size() >= checksum_offset(request.checksum) + 2) &&
                   "a segment shorter than its checksum field");
            device::MacAddress next_mac{};
            if (is_broadcast(request.destination)) {
                next_mac = device::MacAddress::broadcast();
            } else if (is_multicast(request.destination)) {
                next_mac = multicast_mac(request.destination);
            } else {
                device::Ipv4Address next_hop = request.destination;
                if (!on_subnet(next_hop)) {
                    if (!config_.gateway.has_value()) {
                        ++counters_.send_no_route;
                        return std::unexpected{SendError::NoRoute};
                    }
                    next_hop = *config_.gateway;
                }
                const std::optional<device::MacAddress> mac = resolve(next_hop, now);
                if (!mac) {
                    ++counters_.send_unresolved;
                    return std::unexpected{SendError::Unresolved};
                }
                next_mac = *mac;
            }
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
            const Ipv4Header header{.total_length   = static_cast<std::uint16_t>(ipv4_header_size + segment_size),
                                    .identification = next_identification(),
                                    .ttl            = config_.ttl,
                                    .protocol       = request.protocol,
                                    .source         = config_.address,
                                    .destination    = request.destination};
            write_ethernet(
                frame.first(ethernet_header_size),
                {.destination = next_mac, .source = queue_->mac(), .ethertype = std::to_underlying(EtherType::Ipv4)});
            write_ipv4(frame.subspan(ethernet_header_size, ipv4_header_size), header);
            packet.set_tx(finish_checksums(frame, header, request.checksum));
            if (queue_->transmit(std::move(packet))) {
                ++counters_.datagrams_sent;
                return {};
            }
            // ShardQueue::transmit moves only on success, so the packet is still ours: hand it back as built.
            // NOLINTBEGIN(bugprone-use-after-move)
            if (request.checksum != device::L4Checksum::None) {
                device::store_be16(frame.subspan(headers_size + checksum_offset(request.checksum), 2), 0);
            }
            packet.trim_front(headers_size);
            packet.set_tx(device::TxMetadata{});
            // NOLINTEND(bugprone-use-after-move)
            ++counters_.send_refused;
            return std::unexpected{SendError::Refused};
        }

        /// The next hop's MAC from the cache, or nothing with a request on its way: how a program pre-resolves its
        /// router.
        [[nodiscard]] std::optional<device::MacAddress> resolve(const device::Ipv4Address next_hop,
                                                                const TimePoint now) noexcept {
            const ArpCache::Lookup found = arp_.lookup(next_hop, now);
            if (found.send_request) {
                request_arp(next_hop, found.mac);
            }
            return found.mac;
        }

        /// A static entry, or a resolution another shard learned. Appends nothing to `resolved()`.
        void learn(const device::Ipv4Address address, const device::MacAddress mac, const TimePoint now) noexcept {
            arp_.learn(address, mac, now);
        }

        [[nodiscard]] device::Ipv4Address address() const noexcept {
            return config_.address;
        }

        [[nodiscard]] std::uint8_t prefix() const noexcept {
            return config_.prefix;
        }

        [[nodiscard]] std::optional<device::Ipv4Address> gateway() const noexcept {
            return config_.gateway;
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return queue_->mtu();
        }

        /// What one `send` may carry: the MTU less the IPv4 header. TCP's MSS starts here.
        [[nodiscard]] std::uint16_t max_l4_size() const noexcept {
            return static_cast<std::uint16_t>(queue_->mtu() - ipv4_header_size);
        }

        [[nodiscard]] const Ipv4Counters& counters() const noexcept {
            return counters_;
        }

    private:
        static constexpr std::uint16_t identification_stride =
            4096;  ///< Per queue index, so shards do not collide early.

        [[nodiscard]] static constexpr std::uint32_t mask_of(const std::uint8_t prefix) noexcept {
            return prefix == 0 ? 0U : ~std::uint32_t{0} << (32U - prefix);
        }

        [[nodiscard]] static const Ipv4Config& validated(const Ipv4Config& config) {
            if (config.address == device::Ipv4Address{}) {
                throw std::invalid_argument{"Ipv4Config::address must not be zero"};
            }
            if (config.prefix > 32) {
                throw std::invalid_argument{"Ipv4Config::prefix must be at most 32"};
            }
            if (config.ttl == 0) {
                throw std::invalid_argument{"Ipv4Config::ttl must be positive"};
            }
            if (config.burst_capacity == 0) {
                throw std::invalid_argument{"Ipv4Config::burst_capacity must be positive"};
            }
            if (config.gateway.has_value() &&
                ((config.gateway->to_uint32() ^ config.address.to_uint32()) & mask_of(config.prefix)) != 0) {
                throw std::invalid_argument{"Ipv4Config::gateway must be on the subnet"};
            }
            return config;  // the ARP capacity and durations are ArpCache's to check
        }

        [[nodiscard]] bool on_subnet(const device::Ipv4Address address) const noexcept {
            return ((address.to_uint32() ^ config_.address.to_uint32()) & mask_) == 0;
        }

        [[nodiscard]] device::Ipv4Address subnet_broadcast() const noexcept {
            return device::Ipv4Address::from_uint32(config_.address.to_uint32() | ~mask_);
        }

        /// The limited broadcast, or the subnet broadcast where the subnet has one.
        [[nodiscard]] bool is_broadcast(const device::Ipv4Address address) const noexcept {
            return address == limited_broadcast || (config_.prefix <= 30 && address == subnet_broadcast());
        }

        [[nodiscard]] static bool is_multicast(const device::Ipv4Address address) noexcept {
            return (std::to_integer<unsigned>(address.bytes()[0]) & 0xf0U) == 0xe0U;
        }

        /// False for a sender that cannot be a host: zero, multicast, the limited broadcast, or a group MAC.
        [[nodiscard]] static bool plausible_sender(const ArpPacket& arp) noexcept {
            return arp.sender_ip != device::Ipv4Address{} && !is_multicast(arp.sender_ip) &&
                   arp.sender_ip != limited_broadcast && !arp.sender_mac.is_multicast();
        }

        [[nodiscard]] bool for_me(const device::Ipv4Address destination) const noexcept {
            return destination == config_.address || is_broadcast(destination);
        }

        [[nodiscard]] std::uint16_t next_identification() noexcept {
            return identification_++;
        }

        void receive(Packet& packet, const TimePoint now) noexcept {
            const std::span<std::byte> frame             = packet.data();
            const std::optional<EthernetHeader> ethernet = parse_ethernet(frame);
            if (!ethernet) {
                ++counters_.dropped_short;
                return;
            }
            if (ethernet->destination != queue_->mac() && !ethernet->destination.is_broadcast()) {
                ++counters_.dropped_not_for_us;
                return;
            }
            if (ethernet->ethertype == std::to_underlying(EtherType::Arp)) {
                receive_arp(packet, now);
                return;
            }
            if (ethernet->ethertype == std::to_underlying(EtherType::Ipv4)) {
                receive_ipv4(packet, *ethernet);
                return;
            }
            ++counters_.dropped_ethertype;
        }

        void receive_arp(Packet& packet, const TimePoint now) noexcept {
            const std::span<std::byte> frame   = packet.data();
            const std::optional<ArpPacket> arp = parse_arp(frame.subspan(ethernet_header_size));
            if (!arp) {
                ++counters_.dropped_arp_malformed;
                return;
            }
            if (arp->sender_ip == config_.address) {
                ++counters_.dropped_arp_conflict;
                return;
            }
            if (arp->operation == ArpOperation::Request) {
                ++counters_.arp_requests_received;
                const bool plausible = plausible_sender(*arp);
                if (arp->target_ip != config_.address) {
                    // Someone else's business; `process` releases it. RFC 826's merge rule: an entry we hold is
                    // refreshed, a new one is never created from a request not meant for us.
                    if (plausible && arp_.contains(arp->sender_ip)) {
                        learn_from_wire(arp->sender_ip, arp->sender_mac, now);
                    }
                    return;
                }
                if (plausible) {  // a probe (RFC 5227) is still answered, but teaches nothing
                    learn_from_wire(arp->sender_ip, arp->sender_mac, now);
                }
                write_ethernet(frame.first(ethernet_header_size),
                               {.destination = arp->sender_mac,
                                .source      = queue_->mac(),
                                .ethertype   = std::to_underlying(EtherType::Arp)});
                write_arp(frame.subspan(ethernet_header_size),
                          {.operation  = ArpOperation::Reply,
                           .sender_mac = queue_->mac(),
                           .sender_ip  = config_.address,
                           .target_mac = arp->sender_mac,
                           .target_ip  = arp->sender_ip});
                packet.set_tx(device::TxMetadata{});
                transmit_built(std::move(packet), counters_.arp_replies_sent);
                return;
            }
            ++counters_.arp_replies_received;
            if (!arp_.contains(arp->sender_ip)) {
                ++counters_.dropped_arp_unsolicited;  // never asked: nothing on the segment fills the cache
                return;
            }
            if (plausible_sender(*arp)) {
                learn_from_wire(arp->sender_ip, arp->sender_mac, now);
            }
        }

        void receive_ipv4(Packet& packet, const EthernetHeader& ethernet) noexcept {
            const std::span<std::byte> frame       = packet.data();
            const std::span<std::byte> payload     = frame.subspan(ethernet_header_size);
            const std::optional<Ipv4Header> header = parse_ipv4(payload);
            if (!header) {
                ++counters_.dropped_bad_header;
                return;
            }
            const device::RxMetadata rx = packet.rx();
            const bool bad_checksum     = rx.l3 == device::ChecksumVerdict::Bad ||
                                          (rx.l3 == device::ChecksumVerdict::Unknown &&
                                           device::internet_checksum(payload.first(header->header_length)) != 0);
            if (bad_checksum) {
                ++counters_.dropped_bad_checksum;
                return;
            }
            if (is_multicast(header->source) || header->source == limited_broadcast ||
                std::to_integer<unsigned>(header->source.bytes()[0]) == 127U) {
                ++counters_.dropped_martian;
                return;
            }
            if (!for_me(header->destination)) {
                ++counters_.dropped_not_for_us;
                return;
            }
            if (header->is_fragment()) {
                ++counters_.dropped_fragment;
                return;
            }
            ++counters_.datagrams_received;
            switch (header->protocol) {
                case device::Ipv4Protocol::Tcp:
                    deliver(tcp_, packet, *header, rx);
                    ++counters_.delivered_tcp;
                    return;
                case device::Ipv4Protocol::Udp:
                    deliver(udp_, packet, *header, rx);
                    ++counters_.delivered_udp;
                    return;
                case device::Ipv4Protocol::Icmp:
                    receive_icmp(packet, ethernet, *header);
                    return;
                default:
                    ++counters_.dropped_protocol;
                    return;
            }
        }

        void deliver(std::vector<Datagram<Packet>>& list,
                     Packet& packet,
                     const Ipv4Header& header,
                     const device::RxMetadata& rx) noexcept {
            list.push_back(
                Datagram<Packet>{.packet      = std::move(packet),
                                 .source      = header.source,
                                 .destination = header.destination,
                                 .protocol    = header.protocol,
                                 .l3_offset   = static_cast<std::uint8_t>(ethernet_header_size),
                                 .l3_length   = header.header_length,
                                 .l4_length   = static_cast<std::uint16_t>(header.total_length - header.header_length),
                                 .l4_checksum = rx.l4});
        }

        void
        learn_from_wire(const device::Ipv4Address address, const device::MacAddress mac, const TimePoint now) noexcept {
            arp_.learn(address, mac, now);
            resolved_.push_back(ArpResolution{.address = address, .mac = mac});
            ++counters_.resolutions;
        }

        /// A request for `target`: broadcast for an unresolved entry, unicast to `known` for a stale one.
        void request_arp(const device::Ipv4Address target, const std::optional<device::MacAddress> known) noexcept {
            std::optional<Packet> packet = queue_->allocate();
            if (!packet) {
                ++counters_.transmit_refused;
                return;
            }
            const std::optional<std::span<std::byte>> room = packet->append(ethernet_header_size + arp_packet_size);
            if (!room) {
                ++counters_.transmit_refused;
                return;
            }
            write_ethernet(room->first(ethernet_header_size),
                           {.destination = known.value_or(device::MacAddress::broadcast()),
                            .source      = queue_->mac(),
                            .ethertype   = std::to_underlying(EtherType::Arp)});
            write_arp(room->subspan(ethernet_header_size),
                      {.operation  = ArpOperation::Request,
                       .sender_mac = queue_->mac(),
                       .sender_ip  = config_.address,
                       .target_mac = {},
                       .target_ip  = target});
            packet->set_tx(device::TxMetadata{});
            transmit_built(std::move(*packet), counters_.arp_requests_sent);
        }

        /// Transmits a frame the brick built: counts it under `sent`, or as refused.
        void transmit_built(Packet&& packet, std::uint64_t& sent) noexcept {
            if (queue_->transmit(std::move(packet))) {
                ++sent;
            } else {
                ++counters_.transmit_refused;
            }
        }

        static constexpr std::size_t tcp_checksum_offset = 16;
        static constexpr std::size_t udp_checksum_offset = 6;

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
        [[nodiscard]] device::TxMetadata
        finish_checksums(std::span<std::byte> frame, const Ipv4Header& header, const device::L4Checksum l4) noexcept {
            device::TxMetadata tx{.l2_length          = static_cast<std::uint8_t>(ethernet_header_size),
                                  .l3_length          = static_cast<std::uint8_t>(ipv4_header_size),
                                  .fill_ipv4_checksum = false,
                                  .fill_l4_checksum   = device::L4Checksum::None};
            const std::span<std::byte> ip      = frame.subspan(ethernet_header_size, ipv4_header_size);
            const std::span<std::byte> segment = frame.subspan(headers_size, header.total_length - ipv4_header_size);
            if (l4 != device::L4Checksum::None) {
                const std::span<std::byte> field = segment.subspan(checksum_offset(l4), 2);
                assert(device::load_be16(field) == 0 && "the caller leaves the L4 checksum field zero");
                if (queue_->capabilities().tx_l4_checksum) {
                    device::store_be16(field,
                                       device::ipv4_pseudo_header_sum(header.source,
                                                                      header.destination,
                                                                      header.protocol,
                                                                      static_cast<std::uint16_t>(segment.size())));
                    tx.fill_l4_checksum = l4;
                } else {
                    std::uint16_t sum =
                        device::ipv4_l4_checksum(header.source, header.destination, header.protocol, segment);
                    if (l4 == device::L4Checksum::Udp && sum == 0) {
                        sum = 0xffff;  // zero means "no checksum" in UDP
                    }
                    device::store_be16(field, sum);
                }
            }
            if (queue_->capabilities().tx_ipv4_checksum) {
                tx.fill_ipv4_checksum = true;  // the field is already zero
            } else {
                device::store_be16(ip.subspan(device::ipv4_checksum_offset, 2), device::ipv4_header_checksum(ip));
            }
            return tx;
        }

        /// An echo request to our unicast address becomes the reply, in the same packet, back to the frame's source
        /// MAC.
        void receive_icmp(Packet& packet, const EthernetHeader& ethernet, const Ipv4Header& header) noexcept {
            const std::span<std::byte> frame = packet.data();
            const std::size_t message_length = header.total_length - header.header_length;
            const std::span<std::byte> message =
                frame.subspan(ethernet_header_size + header.header_length, message_length);
            const std::optional<IcmpHeader> icmp = parse_icmp(message);
            if (!icmp) {
                ++counters_.dropped_bad_header;
                return;
            }
            if (device::internet_checksum(message) != 0) {  // no card offloads ICMP: always software
                ++counters_.dropped_bad_checksum;
                return;
            }
            if (icmp->type != IcmpType::EchoRequest || icmp->code != 0 || header.destination != config_.address) {
                ++counters_.dropped_icmp;  // other types, and echoes to a broadcast address, as the kernel does
                return;
            }
            // Aloe never sends options: drop them from the front. The message already sits after them and stays put.
            packet.trim_front(header.header_length - ipv4_header_size);
            const std::span<std::byte> reply = packet.data();
            const Ipv4Header reply_header{.total_length = static_cast<std::uint16_t>(ipv4_header_size + message_length),
                                          .identification = next_identification(),
                                          .ttl            = config_.ttl,
                                          .protocol       = device::Ipv4Protocol::Icmp,
                                          .source         = config_.address,
                                          .destination    = header.source};
            write_ethernet(reply.first(ethernet_header_size),
                           {.destination = ethernet.source,  // the last hop is the next hop back, router or not
                            .source      = queue_->mac(),
                            .ethertype   = std::to_underlying(EtherType::Ipv4)});
            write_ipv4(reply.subspan(ethernet_header_size, ipv4_header_size), reply_header);
            const std::span<std::byte> reply_message = reply.subspan(headers_size, message_length);
            reply_message[0]                         = std::byte{std::to_underlying(IcmpType::EchoReply)};
            device::store_be16(reply_message.subspan(icmp_checksum_offset, 2), 0);
            device::store_be16(reply_message.subspan(icmp_checksum_offset, 2),
                               device::internet_checksum(reply_message));
            packet.set_tx(finish_checksums(reply, reply_header, device::L4Checksum::None));
            transmit_built(std::move(packet), counters_.echo_replies_sent);
        }

        loop::ShardQueue<Device>* queue_;
        Ipv4Config config_;
        std::uint32_t mask_;
        ArpCache arp_;
        std::uint16_t identification_;
        std::vector<Datagram<Packet>> tcp_;
        std::vector<Datagram<Packet>> udp_;
        std::vector<ArpResolution> resolved_;
        Ipv4Counters counters_{};
    };

}  // namespace aloe::net
