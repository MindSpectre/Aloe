#include "fabric.hpp"

#include <algorithm>
#include <aloe/wire>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <impairment.hpp>

namespace aloe::fabric {

    namespace {

        constexpr std::uint16_t fabric_rss_table_size = 128;
        constexpr std::uint16_t minimum_mtu           = 68;
        // A double holds 53 bits of mantissa; the engine's raw 64 bits lose the low eleven, so the draw is the same
        // on every standard library, which std::uniform_real_distribution is not.
        constexpr unsigned discarded_bits             = 11;
        constexpr int mantissa_bits                   = 53;

        [[nodiscard]] device::Capabilities capabilities_of(const PortConfig& config) {
            const bool checksums     = config.offloads == EmulatedOffloads::Checksums;
            const std::size_t larger = std::min<std::size_t>(config.data_capacity - wire::EthernetHeader::size,
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
                .tx_reclaim       = true,
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

        constexpr std::size_t udp_header_size     = 8;  // wire has no UDP format
        constexpr std::size_t udp_checksum_offset = 6;
        constexpr std::size_t ports_size          = 4;  // source and destination port lead both TCP and UDP

        /// The IPv4 header of an Ethernet frame, as wire parses it. Nothing for any other frame, a truncated one
        /// included.
        [[nodiscard]] std::optional<wire::Ipv4Header> ipv4_of(const std::span<const std::byte> frame) noexcept {
            const auto ethernet = wire::EthernetHeader::parse(frame);
            if (!ethernet || ethernet->ethertype != std::to_underlying(wire::EtherType::Ipv4)) {
                return std::nullopt;
            }
            return wire::Ipv4Header::parse(frame.subspan(wire::EthernetHeader::size));
        }

        /// Where the segment starts in the frame. Inside the frame for any header `ipv4_of` returned.
        [[nodiscard]] std::size_t l4_offset_of(const wire::Ipv4Header& ipv4) noexcept {
            return wire::EthernetHeader::size + std::size_t{ipv4.header_length};
        }

        /// How long the total length says the segment is. Inside the frame for any header `ipv4_of` returned.
        [[nodiscard]] std::size_t l4_length_of(const wire::Ipv4Header& ipv4) noexcept {
            return std::size_t{ipv4.total_length} - std::size_t{ipv4.header_length};
        }

        /// The flow a card steers the frame by. A fragment keeps only its addresses. The ports take part only for an
        /// unfragmented TCP or UDP segment of at least `ports_size` bytes.
        [[nodiscard]] device::FlowTuple flow_of(const wire::Ipv4Header& ipv4,
                                                const std::span<const std::byte> l4) noexcept {
            device::FlowTuple flow{.source = ipv4.source, .destination = ipv4.destination};
            if (ipv4.is_fragment()) {
                return flow;
            }
            flow.protocol     = ipv4.protocol;
            const bool ported = ipv4.protocol == wire::Ipv4Protocol::Tcp || ipv4.protocol == wire::Ipv4Protocol::Udp;
            if (ported && l4.size() >= ports_size) {
                flow.source_port      = wire::load_be16(l4.first(2));
                flow.destination_port = wire::load_be16(l4.subspan(2, 2));
            }
            return flow;
        }

        /// The offset of the checksum field inside the segment: unfragmented TCP or UDP with a whole header.
        [[nodiscard]] std::optional<std::size_t> l4_checksum_offset(const wire::Ipv4Header& ipv4,
                                                                    const std::size_t l4_length) noexcept {
            if (ipv4.is_fragment()) {
                return std::nullopt;
            }
            if (ipv4.protocol == wire::Ipv4Protocol::Tcp && l4_length >= wire::TcpHeader::size) {
                return wire::TcpHeader::checksum_offset;
            }
            if (ipv4.protocol == wire::Ipv4Protocol::Udp && l4_length >= udp_header_size) {
                return udp_checksum_offset;
            }
            return std::nullopt;
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
        const auto ipv4 = ipv4_of(frame);
        if (!ipv4) {
            return rx;
        }
        const std::span<const std::byte> l4 = frame.subspan(l4_offset_of(*ipv4), l4_length_of(*ipv4));
        const device::FlowTuple flow        = flow_of(*ipv4, l4);
        if (steering_.enabled) {
            rx.rss_hash = device::flow_hash(steering_, flow);
            queue       = device::queue_for(steering_, flow);
        }
        if (config_.offloads == EmulatedOffloads::Checksums) {
            rx.l3 =
                verdict(wire::internet_checksum(frame.subspan(wire::EthernetHeader::size, ipv4->header_length)) == 0);
            if (const auto offset = l4_checksum_offset(*ipv4, l4.size())) {
                const bool udp_without_checksum =
                    ipv4->protocol == wire::Ipv4Protocol::Udp && wire::load_be16(l4.subspan(*offset, 2)) == 0;
                if (!udp_without_checksum) {
                    const std::uint32_t pseudo = wire::ipv4_pseudo_header_sum(
                        ipv4->source, ipv4->destination, ipv4->protocol, static_cast<std::uint16_t>(l4.size()));
                    rx.l4 = verdict(wire::checksum_finish(wire::checksum_add(pseudo, l4)) == 0);
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
        if (frame.size() > static_cast<std::size_t>(config_.mtu) + wire::EthernetHeader::size) {
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
        release_finished(source);
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
        const auto ipv4                  = ipv4_of(frame);
        assert(ipv4.has_value() && "a checksum fill was requested for a frame that is not IPv4");
        if (!ipv4) {
            return;
        }
        // A card finds the headers by the metadata, not by parsing; a wrong length fails there, so it fails here.
        assert(tx.l2_length == wire::EthernetHeader::size &&
               "device::TxMetadata::l2_length is not the Ethernet header");
        assert(tx.l3_length == ipv4->header_length && "device::TxMetadata::l3_length is not the IPv4 header length");
        assert((tx.fill_l4_checksum != device::L4Checksum::Tcp || ipv4->protocol == wire::Ipv4Protocol::Tcp) &&
               "a TCP checksum was requested for a segment that is not TCP");
        assert((tx.fill_l4_checksum != device::L4Checksum::Udp || ipv4->protocol == wire::Ipv4Protocol::Udp) &&
               "a UDP checksum was requested for a segment that is not UDP");
        assert((!tx.fill_ipv4_checksum ||
                wire::load_be16(frame.subspan(wire::EthernetHeader::size + wire::ipv4_checksum_offset, 2)) == 0) &&
               "an IPv4 checksum fill needs zero in the field");
        if (tx.fill_ipv4_checksum) {
            const std::uint16_t checksum =
                wire::ipv4_header_checksum(frame.subspan(wire::EthernetHeader::size, ipv4->header_length));
            wire::store_be16(frame.subspan(wire::EthernetHeader::size + wire::ipv4_checksum_offset, 2), checksum);
        }
        if (tx.fill_l4_checksum != device::L4Checksum::None) {
            const std::span<std::byte> l4 = frame.subspan(l4_offset_of(*ipv4), l4_length_of(*ipv4));
            if (const auto offset = l4_checksum_offset(*ipv4, l4.size())) {
                // The field holds the pseudo-header sum; summing the segment as it is completes it.
                std::uint16_t checksum = wire::internet_checksum(l4);
                if (ipv4->protocol == wire::Ipv4Protocol::Udp && checksum == 0) {
                    checksum = 0xffff;  // zero means "no checksum" in UDP
                }
                wire::store_be16(l4.subspan(*offset, 2), checksum);
            }
        }
    }

    std::size_t Port::transmit(const std::uint16_t queue, std::span<Packet> in) noexcept {
        assert(queue < queues_.size());
        Queue& source = *queues_[queue];
        {
            const std::lock_guard lock{source.mutex};
            release_finished(source);
        }
        for (Packet& packet : in) {
            if (packet.empty()) {
                continue;
            }
            fill_checksums(packet);
            const std::span<const std::byte> frame = packet.data();
            if (frame.size() < wire::EthernetHeader::size ||
                frame.size() > static_cast<std::size_t>(config_.mtu) + wire::EthernetHeader::size) {
                const std::lock_guard lock{source.mutex};
                ++source.counters.oversized;
            } else {
                egress(frame);
                const std::lock_guard lock{source.mutex};
                ++source.counters.transmitted;
                if (packet.borrowed()) {
                    source.in_flight.push_back(std::move(packet));
                }
            }
            packet = Packet{};
        }
        return in.size();
    }

    void Port::set_impairment(const Impairment& impairment) {
        const std::lock_guard lock{impairment_mutex_};
        impairment_ = impairment;
        engine_.seed(impairment.seed);
    }

    ImpairmentCounters Port::impairment_counters() const noexcept {
        const std::lock_guard lock{impairment_mutex_};
        return impairment_counters_;
    }

    double Port::draw() noexcept {
        return std::ldexp(static_cast<double>(engine_() >> discarded_bits), -mantissa_bits);
    }

    Verdict Port::judge(const std::span<const std::byte> frame) {
        const std::lock_guard lock{impairment_mutex_};
        const Egress egress{.index = egress_index_++, .frame = frame};
        if (impairment_.script) {
            if (const std::optional<Verdict> scripted = impairment_.script(egress)) {
                return *scripted;
            }
        }
        if (impairment_.loss > 0 && draw() < impairment_.loss) {
            return Verdict{.action = Action::Drop};
        }
        if (impairment_.duplicate > 0 && draw() < impairment_.duplicate) {
            return Verdict{.action = Action::Duplicate};
        }
        if (impairment_.reorder > 0 && draw() < impairment_.reorder) {
            return Verdict{.action = Action::Reorder};
        }
        if (impairment_.delay > core::Duration{} || impairment_.jitter > core::Duration{}) {
            return Verdict{.action = Action::Delay,
                           .delay  = impairment_.delay +
                                     std::chrono::duration_cast<core::Duration>(impairment_.jitter * draw())};
        }
        return Verdict{};
    }

    void Port::egress(const std::span<const std::byte> frame) {
        const Verdict verdict = judge(frame);
        switch (verdict.action) {
            case Action::Deliver:
                emit(frame);
                return;
            case Action::Drop: {
                const std::lock_guard lock{impairment_mutex_};
                ++impairment_counters_.dropped;
                return;
            }
            case Action::Duplicate: {
                fabric_.deliver(*this, frame);
                fabric_.deliver(*this, frame);
                release_hold();
                const std::lock_guard lock{impairment_mutex_};
                ++impairment_counters_.duplicated;
                return;
            }
            case Action::Reorder: {
                std::optional<std::vector<std::byte>> previous;
                {
                    const std::lock_guard lock{impairment_mutex_};
                    ++impairment_counters_.reordered;
                    previous = std::move(hold_);
                    hold_    = std::vector<std::byte>(frame.begin(), frame.end());
                }
                if (previous) {
                    fabric_.deliver(*this, *previous);
                }
                return;
            }
            case Action::Delay: {
                {
                    const std::lock_guard lock{impairment_mutex_};
                    ++impairment_counters_.delayed;
                }
                fabric_.delay(*this, frame, verdict.delay);
                return;
            }
        }
        std::unreachable();
    }

    void Port::emit(const std::span<const std::byte> frame) {
        fabric_.deliver(*this, frame);
        release_hold();
    }

    void Port::release_hold() {
        std::optional<std::vector<std::byte>> held;
        {
            const std::lock_guard lock{impairment_mutex_};
            held = std::move(hold_);
            hold_.reset();
        }
        if (held) {
            fabric_.deliver(*this, *held);
        }
    }

    void Port::release_finished(Queue& queue) const noexcept {
        if (config_.loans != LoanReturn::OnStep) {
            return;
        }
        const auto finished = static_cast<std::ptrdiff_t>(queue.finished);
        queue.in_flight.erase(queue.in_flight.begin(), queue.in_flight.begin() + finished);
        queue.finished = 0;
    }

    std::size_t Port::reclaim(const std::uint16_t queue) noexcept {
        assert(queue < queues_.size());
        Queue& source = *queues_[queue];
        const std::lock_guard lock{source.mutex};
        if (config_.loans != LoanReturn::OnStep) {
            return 0;
        }
        const std::size_t count = source.in_flight.size();
        source.in_flight.clear();
        source.finished = 0;
        return count;
    }

    void Port::return_loans(const std::uint16_t queue) noexcept {
        assert(queue < queues_.size());
        Queue& source = *queues_[queue];
        const std::lock_guard lock{source.mutex};
        source.in_flight.clear();
        source.finished = 0;
    }

    std::size_t Port::loans_outstanding(const std::uint16_t queue) const noexcept {
        assert(queue < queues_.size());
        const std::lock_guard lock{queues_[queue]->mutex};
        return queues_[queue]->in_flight.size();
    }

    Port& Fabric::add_port(const PortConfig& config) {
        if (config.queues == 0) {
            throw std::invalid_argument{"a fabric port needs at least one queue"};
        }
        if (config.data_capacity < static_cast<std::size_t>(config.mtu) + wire::EthernetHeader::size) {
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

    void Fabric::advance(const core::TimePoint now) {
        std::vector<DelayedFrame> due;
        {
            const std::lock_guard lock{mutex_};
            if (now < now_) {
                return;
            }
            now_ = now;
            const auto later =
                std::ranges::stable_partition(delayed_, [this](const DelayedFrame& entry) { return entry.due > now_; });
            due.assign(std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
            delayed_.erase(later.begin(), later.end());
        }
        std::ranges::sort(due, [](const DelayedFrame& left, const DelayedFrame& right) {
            return left.due != right.due ? left.due < right.due : left.sequence < right.sequence;
        });
        // Delivery takes destination queue locks, so no fabric or impairment lock is held from here on.
        for (const DelayedFrame& entry : due) {
            deliver(*entry.source, entry.bytes);
        }
        for (const auto& port : ports_) {
            port->release_hold();
        }
        for (const auto& port : ports_) {
            if (port->config_.loans != LoanReturn::OnStep) {
                continue;
            }
            for (const auto& queue : port->queues_) {
                const std::lock_guard lock{queue->mutex};
                queue->finished = queue->in_flight.size();
            }
        }
    }

    void Fabric::delay(Port& source, const std::span<const std::byte> frame, const core::Duration by) {
        const std::lock_guard lock{mutex_};
        delayed_.push_back(DelayedFrame{.due      = now_ + by,
                                        .sequence = delayed_sequence_++,
                                        .source   = &source,
                                        .bytes    = std::vector<std::byte>(frame.begin(), frame.end())});
    }

    core::TimePoint Fabric::now() const noexcept {
        const std::lock_guard lock{mutex_};
        return now_;
    }

    void Fabric::deliver(const Port& source, const std::span<const std::byte> frame) {
        assert(frame.size() >= wire::EthernetHeader::size);
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
