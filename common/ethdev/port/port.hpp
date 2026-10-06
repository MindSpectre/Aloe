#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <device.hpp>
#include <ethdev_packet.hpp>
#include <mac_address.hpp>
#include <rss.hpp>

struct rte_mempool;
struct rte_eth_dev_info;

namespace aloe::ethdev {

    struct PortConfig {
        std::string name;  ///< As DPDK names it: `net_ring0`, `net_tap0`, `0000:03:00.0`.
        std::uint16_t queues      = 1;
        std::uint16_t mtu         = 1500;
        std::uint16_t descriptors = 1024;  ///< Per receive queue and per transmit queue.
        std::uint32_t pool_size   = 4096;  ///< Mbufs per queue.
    };

    /**
     * @brief One DPDK port with N queues. Models IsDevice.
     *
     * Construction looks the port up by name, configures it with the requested queues, the MTU,
     * every checksum offload the driver has and receive-side scaling under Aloe's key when the
     * driver has that, and starts it. It claims the device through DPDK's ownership API first, so a
     * second Port on the same name fails without touching it. A construction that fails after that
     * closes the port, and DPDK cannot reopen a closed port. Destruction stops and closes the port and frees its
     * pools; every packet must be gone by then. The port does not know which driver is behind it.
     */
    class Port {
    public:
        using Packet = ethdev::Packet;

        /// Packets a single receive or transmit call moves at most.
        static constexpr std::size_t max_burst = 64;

        explicit Port(const PortConfig& config);
        Port(const Port&)            = delete;
        Port& operator=(const Port&) = delete;
        Port(Port&&)                 = delete;
        Port& operator=(Port&&)      = delete;
        ~Port();

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return queues_;
        }

        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return mac_;
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return mtu_;
        }

        [[nodiscard]] bool link_up() const noexcept;

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return capabilities_;
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return steering_;
        }

        [[nodiscard]] std::optional<Packet> allocate(std::uint16_t queue) noexcept;
        [[nodiscard]] std::size_t receive(std::uint16_t queue, std::span<Packet> out) noexcept;
        [[nodiscard]] std::size_t transmit(std::uint16_t queue, std::span<Packet> in) noexcept;

        /// Read from the queue's own thread. `dropped` stays zero: DPDK reports receive-ring
        /// overruns per port, in `rte_eth_stats`, not per queue.
        [[nodiscard]] device::QueueCounters counters(std::uint16_t queue) const noexcept;

        [[nodiscard]] std::uint16_t port_id() const noexcept {
            return port_id_;
        }

        [[nodiscard]] std::string_view driver_name() const noexcept {
            return driver_;
        }

    private:
        void configure(const PortConfig& config, const rte_eth_dev_info& info);
        void program_rss(const rte_eth_dev_info& info);
        void teardown() noexcept;

        std::uint64_t owner_   = 0;
        std::uint16_t port_id_ = 0;
        std::uint16_t queues_  = 0;
        std::uint16_t mtu_     = 0;
        bool owned_            = false;
        bool started_          = false;
        wire::MacAddress mac_;
        std::string driver_;
        device::Capabilities capabilities_;
        device::RssDescription steering_;
        std::vector<rte_mempool*> pools_;
        std::vector<device::QueueCounters> counters_;
    };

}  // namespace aloe::ethdev
