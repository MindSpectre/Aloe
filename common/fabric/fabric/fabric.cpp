#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <bytes.hpp>
#include <checksum.hpp>
#include <detail/frame.hpp>
#include <ethernet.hpp>
#include <fabric.hpp>
#include <ipv4_checksum.hpp>
#include <ipv4_protocol.hpp>
#include <mac_address.hpp>

namespace aloe::fabric {

    namespace {

        constexpr std::uint16_t fabric_rss_table_size = 128;
        constexpr std::uint16_t minimum_mtu           = 68;

        [[nodiscard]] device::Capabilities capabilities_of(const PortConfig& config) {
            const bool checksums     = config.offloads == EmulatedOffloads::Checksums;
            const std::size_t larger = std::min<std::size_t>(config.data_capacity - wire::ethernet_header_size,
                                                             std::numeric_limits<std::uint16_t>::max());
            return device::Capabilities{
                .max_rx_queues  = config.queues,
                .max_tx_queues  = config.queues,
                .min_mtu        = minimum_mtu,
                .max_mtu        = static_cast<std::uint16_t>(larger),
                .rss            = config.queues > 1,
                .rss_key_size   = device::rss_key_capacity,
                .rss_table_size = fabric_rss_table_size,
                .rss_types = config.queues > 1 ? device::RssHashTypes{.ipv4 = true, .ipv4_tcp = true, .ipv4_udp = true}
                                               : device::RssHashTypes{},
                .rx_ipv4_checksum = checksums,
                .rx_l4_checksum   = checksums,
                .tx_ipv4_checksum = checksums,
                .tx_l4_checksum   = checksums,
            };
        }

        [[nodiscard]] device::RssDescription steering_of(const PortConfig& config) {
            if (config.queues > 1) {
                return device::round_robin_rss(config.queues, fabric_rss_table_size);
            }
            return device::RssDescription{};
        }

        [[nodiscard]] device::ChecksumVerdict verdict(const bool good) noexcept {
            return good ? device::ChecksumVerdict::Good : device::ChecksumVerdict::Bad;
        }

    }  // namespace

    Port::Port(PrivateTag, Fabric& fabric, const PortConfig& config)
        : fabric_{fabric},
          config_{config},
          capabilities_{capabilities_of(config)},
          steering_{steering_of(config)} {
        queues_.reserve(config.queues);
        for (std::uint16_t queue = 0; queue < config.queues; ++queue) {
            queues_.push_back(std::make_unique<Queue>(config.pool_size, config.data_capacity));
        }
    }

    Port::~Port() = default;

    std::optional<Packet> Port::allocate(const std::uint16_t queue) noexcept {
        assert(queue < queues_.size());
        return queues_[queue]->pool.allocate();
    }

    device::QueueCounters Port::counters(const std::uint16_t queue) const noexcept {
        assert(queue < queues_.size());
        const std::lock_guard lock{queues_[queue]->mutex};
        return queues_[queue]->counters;
    }

    device::RxMetadata Port::inspect(const std::span<const std::byte> frame, std::uint16_t& queue) const noexcept {
        device::RxMetadata rx;
        queue           = 0;
        const auto ipv4 = detail::parse_ipv4(frame);
        if (!ipv4) {
            return rx;
        }
        const device::FlowTuple flow = detail::flow_of(*ipv4);
        if (steering_.enabled) {
            rx.rss_hash = device::flow_hash(steering_, flow);
            queue       = device::queue_for(steering_, flow);
        }
        if (config_.offloads == EmulatedOffloads::Checksums) {
            rx.l3 = verdict(wire::internet_checksum(ipv4->header(frame)) == 0);
            if (const auto offset = ipv4->l4_checksum_offset()) {
                const bool udp_without_checksum =
                    ipv4->protocol == wire::Ipv4Protocol::Udp && wire::load_be16(frame.subspan(*offset, 2)) == 0;
                if (!udp_without_checksum) {
                    const std::uint32_t pseudo = wire::ipv4_pseudo_header_sum(
                        ipv4->source, ipv4->destination, ipv4->protocol, static_cast<std::uint16_t>(ipv4->l4_length));
                    rx.l4 = verdict(wire::checksum_finish(wire::checksum_add(pseudo, ipv4->l4(frame))) == 0);
                }
            }
        }
        return rx;
    }

    void Port::deliver(const std::span<const std::byte> frame) {
        std::uint16_t queue         = 0;
        const device::RxMetadata rx = inspect(frame, queue);
        Queue& destination          = *queues_[queue];
        const std::lock_guard lock{destination.mutex};
        if (frame.size() > static_cast<std::size_t>(config_.mtu) + wire::ethernet_header_size) {
            ++destination.counters.oversized;
            return;
        }
        if (destination.pending.size() >= config_.queue_depth) {
            ++destination.counters.dropped;
            return;
        }
        destination.pending.push_back(
            PendingFrame{.bytes = std::vector<std::byte>(frame.begin(), frame.end()), .rx = rx});
    }

    std::size_t Port::receive(const std::uint16_t queue, std::span<Packet> out) noexcept {
        assert(queue < queues_.size());
        Queue& source = *queues_[queue];
        const std::lock_guard lock{source.mutex};
        std::size_t count = 0;
        while (count < out.size() && !source.pending.empty()) {
            std::optional<Packet> packet = source.pool.allocate();
            if (!packet) {
                break;
            }
            assert(out[count].empty() && "receive writes only into empty slots");
            const PendingFrame& frame = source.pending.front();
            const auto room           = packet->append(frame.bytes.size());
            assert(room.has_value() && "add_port guarantees a packet holds a maximum-size frame");
            std::ranges::copy(frame.bytes, room->begin());
            packet->set_rx(frame.rx);
            out[count] = std::move(*packet);
            ++count;
            source.pending.pop_front();
            ++source.counters.received;
        }
        return count;
    }

    void Port::fill_checksums(Packet& packet) const noexcept {
        const device::TxMetadata tx = packet.tx();
        if (!tx.fill_ipv4_checksum && tx.fill_l4_checksum == device::L4Checksum::None) {
            return;
        }
        assert(config_.offloads == EmulatedOffloads::Checksums &&
               "a fill was requested from a port without the capability");
        if (config_.offloads != EmulatedOffloads::Checksums) {
            return;
        }
        const std::span<std::byte> frame = packet.data();
        const auto ipv4                  = detail::parse_ipv4(frame);
        assert(ipv4.has_value() && "a checksum fill was requested for a frame that is not IPv4");
        if (!ipv4) {
            return;
        }
        // A card finds the headers by the metadata, not by parsing; a wrong length fails there, so it fails here.
        assert(tx.l2_length == wire::ethernet_header_size &&
               "device::TxMetadata::l2_length is not the Ethernet header");
        assert(tx.l3_length == ipv4->header_length && "device::TxMetadata::l3_length is not the IPv4 header length");
        assert((tx.fill_l4_checksum != device::L4Checksum::Tcp || ipv4->protocol == wire::Ipv4Protocol::Tcp) &&
               "a TCP checksum was requested for a segment that is not TCP");
        assert((tx.fill_l4_checksum != device::L4Checksum::Udp || ipv4->protocol == wire::Ipv4Protocol::Udp) &&
               "a UDP checksum was requested for a segment that is not UDP");
        assert((!tx.fill_ipv4_checksum ||
                wire::load_be16(frame.subspan(wire::ethernet_header_size + wire::ipv4_checksum_offset, 2)) == 0) &&
               "an IPv4 checksum fill needs zero in the field");
        if (tx.fill_ipv4_checksum) {
            const std::uint16_t checksum = wire::ipv4_header_checksum(ipv4->header(frame));
            wire::store_be16(frame.subspan(wire::ethernet_header_size + wire::ipv4_checksum_offset, 2), checksum);
        }
        if (tx.fill_l4_checksum != device::L4Checksum::None) {
            if (const auto offset = ipv4->l4_checksum_offset()) {
                // The field holds the pseudo-header sum; summing the segment as it is completes it.
                std::uint16_t checksum = wire::internet_checksum(ipv4->l4(frame));
                if (ipv4->protocol == wire::Ipv4Protocol::Udp && checksum == 0) {
                    checksum = 0xffff;  // zero means "no checksum" in UDP
                }
                wire::store_be16(frame.subspan(*offset, 2), checksum);
            }
        }
    }

    std::size_t Port::transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
        assert(queue < queues_.size());
        Queue& source = *queues_[queue];
        for (Packet& packet : in) {
            if (packet.empty()) {
                continue;
            }
            fill_checksums(packet);
            const std::span<const std::byte> frame = packet.data();
            if (frame.size() < wire::ethernet_header_size ||
                frame.size() > static_cast<std::size_t>(config_.mtu) + wire::ethernet_header_size) {
                const std::lock_guard lock{source.mutex};
                ++source.counters.oversized;
            } else {
                fabric_.deliver(*this, frame);
                const std::lock_guard lock{source.mutex};
                ++source.counters.transmitted;
            }
            packet = Packet{};
        }
        return in.size();
    }

    Port& Fabric::add_port(const PortConfig& config) {
        if (config.queues == 0) {
            throw std::invalid_argument{"a fabric port needs at least one queue"};
        }
        if (config.data_capacity < static_cast<std::size_t>(config.mtu) + wire::ethernet_header_size) {
            throw std::invalid_argument{"a fabric port's data capacity must hold a maximum-size frame"};
        }
        for (const auto& port : ports_) {
            if (port->mac() == config.mac) {
                throw std::invalid_argument{"a fabric port with this MAC already exists"};
            }
        }
        ports_.push_back(std::make_unique<Port>(Port::PrivateTag{}, *this, config));
        return *ports_.back();
    }

    void Fabric::deliver(const Port& source, const std::span<const std::byte> frame) {
        assert(frame.size() >= wire::ethernet_header_size);
        const wire::MacAddress destination{
            {frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]},
        };
        if (destination.is_multicast()) {
            for (const auto& port : ports_) {
                if (port.get() != &source) {
                    port->deliver(frame);
                }
            }
            return;
        }
        for (const auto& port : ports_) {
            if (port->mac() == destination) {
                port->deliver(frame);
                return;
            }
        }
    }

}  // namespace aloe::fabric
