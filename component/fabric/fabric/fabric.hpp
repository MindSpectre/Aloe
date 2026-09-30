#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <address.hpp>
#include <device.hpp>
#include <fabric_packet.hpp>
#include <rss.hpp>

namespace aloe::fabric {

    class Fabric;

    /// Which offloads a port pretends to have, and then really performs in software.
    enum class EmulatedOffloads : std::uint8_t {
        None,       ///< No checksum capabilities; every verdict stays Unknown.
        Checksums,  ///< Fills checksums on transmit and verifies them on receive.
    };

    struct PortConfig {
        MacAddress mac;
        std::uint16_t queues      = 1;
        std::uint16_t mtu         = 1500;
        std::size_t data_capacity = 2048;  ///< Bytes of data a packet holds; at least `mtu + 14`.
        std::size_t pool_size     = 1024;  ///< Packets per queue.
        std::size_t queue_depth   = 1024;  ///< Frames a receive queue holds before it drops.
        EmulatedOffloads offloads = EmulatedOffloads::None;
    };

    /**
     * @brief One port of a Fabric. Models IsDevice.
     *
     * Queue i is used from one thread. Any thread may transmit into the port through the fabric;
     * delivery takes the destination queue's lock. Frames are copied on transmit and again on
     * receive, so a pool is only ever touched by its queue's thread.
     */
    class Port {
        struct PrivateTag {};

    public:
        using Packet = fabric::Packet;

        /// Only Fabric::add_port can name the tag, so every port passes its validation.
        Port(PrivateTag, Fabric& fabric, const PortConfig& config);
        Port(const Port&)            = delete;
        Port& operator=(const Port&) = delete;
        Port(Port&&)                 = delete;
        Port& operator=(Port&&)      = delete;
        ~Port();

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return config_.queues;
        }

        [[nodiscard]] MacAddress mac() const noexcept {
            return config_.mac;
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return config_.mtu;
        }

        /// Always up: a fabric has no cables.
        [[nodiscard]] bool link_up() const noexcept {
            return !queues_.empty();
        }

        [[nodiscard]] const Capabilities& capabilities() const noexcept {
            return capabilities_;
        }

        [[nodiscard]] const RssDescription& steering() const noexcept {
            return steering_;
        }

        [[nodiscard]] const PortConfig& config() const noexcept {
            return config_;
        }

        [[nodiscard]] std::optional<Packet> allocate(std::uint16_t queue) noexcept;
        [[nodiscard]] std::size_t receive(std::uint16_t queue, std::span<Packet> out) noexcept;
        [[nodiscard]] std::size_t transmit(std::uint16_t queue, std::span<Packet> in) noexcept;
        [[nodiscard]] QueueCounters counters(std::uint16_t queue) const noexcept;

    private:
        friend class Fabric;

        struct PendingFrame {
            std::vector<std::byte> bytes;
            RxMetadata rx;
        };

        struct Queue {
            Queue(std::size_t pool_size, std::size_t data_capacity)
                : pool{pool_size, data_capacity} {
            }

            mutable std::mutex mutex;
            std::deque<PendingFrame> pending;
            Pool pool;
            QueueCounters counters;
        };

        /// Called by the fabric from the transmitting thread.
        void deliver(std::span<const std::byte> frame);

        void fill_checksums(Packet& packet) const noexcept;
        [[nodiscard]] RxMetadata inspect(std::span<const std::byte> frame, std::uint16_t& queue) const noexcept;

        Fabric& fabric_;
        PortConfig config_;
        Capabilities capabilities_;
        RssDescription steering_;
        std::vector<std::unique_ptr<Queue>> queues_;
    };

    /**
     * @brief An in-memory broadcast domain of ports.
     *
     * Transmit delivers at once: unicast to the port with that MAC (the sender included), broadcast
     * and multicast to every other port. There is no loss, reordering or delay; `deliver` is where
     * a later phase adds them. Add every port before any thread transmits.
     */
    class Fabric {
    public:
        Fabric()                         = default;
        Fabric(const Fabric&)            = delete;
        Fabric& operator=(const Fabric&) = delete;
        Fabric(Fabric&&)                 = delete;
        Fabric& operator=(Fabric&&)      = delete;
        ~Fabric()                        = default;

        /// Throws std::invalid_argument for a duplicate MAC, zero queues, or a data capacity below `mtu + 14`.
        Port& add_port(const PortConfig& config);

        [[nodiscard]] std::size_t port_count() const noexcept {
            return ports_.size();
        }

    private:
        friend class Port;

        void deliver(const Port& source, std::span<const std::byte> frame);

        std::vector<std::unique_ptr<Port>> ports_;
    };

}  // namespace aloe::fabric
