#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <device.hpp>
#include <fabric_packet.hpp>
#include <impairment.hpp>
#include <mac_address.hpp>
#include <rss.hpp>

namespace aloe::fabric {

    class Fabric;

    /// Which offloads a port pretends to have, and then really performs in software.
    enum class EmulatedOffloads : std::uint8_t {
        None,       ///< No checksum capabilities; every verdict stays Unknown.
        Checksums,  ///< Fills checksums on transmit and verifies them on receive.
    };

    /// When a port lets go of a transmitted loan.
    enum class LoanReturn : std::uint8_t {
        OnStep,  ///< Finished by `Fabric::advance`, released by the queue's next transmit, receive or reclaim;
                 ///< `reclaim` releases at once.
        Held,    ///< Only `Port::return_loans` releases; `reclaim` returns zero. The driver that will not let go.
    };

    struct PortConfig {
        wire::MacAddress mac;
        std::uint16_t queues      = 1;
        std::uint16_t mtu         = 1500;
        std::size_t data_capacity = 2048;  ///< Bytes of data a packet holds; at least `mtu + 14`.
        std::size_t pool_size     = 1024;  ///< Packets per queue.
        std::size_t queue_depth   = 1024;  ///< Frames a receive queue holds before it drops.
        EmulatedOffloads offloads = EmulatedOffloads::None;
        LoanReturn loans          = LoanReturn::OnStep;
    };

    /**
     * @brief One port of a Fabric. Models IsDevice.
     *
     * Queue i is used from one thread. Any thread may transmit into the port through the fabric;
     * delivery takes the destination queue's lock. Frames are copied on transmit and again on
     * receive, so a pool is only ever touched by its queue's thread. The copy on transmit is a heap
     * allocation, the one exception to "the hot path allocates nothing but packets": the fabric is
     * a fixture, never a production path, and an allocation failure there terminates the test.
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

        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return config_.mac;
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return config_.mtu;
        }

        /// Always up: a fabric has no cables.
        [[nodiscard]] bool link_up() const noexcept {
            return !queues_.empty();
        }

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return capabilities_;
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return steering_;
        }

        [[nodiscard]] const PortConfig& config() const noexcept {
            return config_;
        }

        [[nodiscard]] std::optional<Packet> allocate(std::uint16_t queue) noexcept;
        [[nodiscard]] std::size_t receive(std::uint16_t queue, std::span<Packet> out) noexcept;
        [[nodiscard]] std::size_t transmit(std::uint16_t queue, std::span<Packet> in) noexcept;
        [[nodiscard]] device::QueueCounters counters(std::uint16_t queue) const noexcept;

        /// Releases every loan of the queue on an OnStep port and returns how many; zero on a Held port. Queue thread.
        [[nodiscard]] std::size_t reclaim(std::uint16_t queue) noexcept;
        /// Releases every loan of the queue whatever the policy. Queue thread; the test's hand on a Held port.
        void return_loans(std::uint16_t queue) noexcept;
        /// Handles transmitted and not yet released.
        [[nodiscard]] std::size_t loans_outstanding(std::uint16_t queue) const noexcept;

        /// Replaces the egress impairment and reseeds its engine. Any thread.
        void set_impairment(const Impairment& impairment);
        [[nodiscard]] ImpairmentCounters impairment_counters() const noexcept;

    private:
        friend class Fabric;

        struct PendingFrame {
            std::vector<std::byte> bytes;
            device::RxMetadata rx;
        };

        struct Queue {
            Queue(const std::size_t pool_size, const std::size_t data_capacity)
                : pool{pool_size, data_capacity} {
            }

            mutable std::mutex mutex;
            std::deque<PendingFrame> pending;
            Pool pool;
            device::QueueCounters counters;
            std::vector<Packet> in_flight;  ///< Borrowed handles, in transmit order; after pool, so they die first.
            std::size_t finished = 0;       ///< The front of in_flight that advance marked finished.
        };

        /// Releases the handles `advance` finished, on an OnStep port. The caller holds the queue's lock.
        void release_finished(Queue& queue) const noexcept;

        /// Called by the fabric from the transmitting thread.
        void deliver(std::span<const std::byte> frame);

        /// The verdict for one frame: the script's, else the model's draws. Takes `impairment_mutex_`.
        [[nodiscard]] Verdict judge(std::span<const std::byte> frame);
        /// Applies the verdict to the frame. Delivers only with no lock held.
        void egress(std::span<const std::byte> frame);
        /// Delivers the frame, then lets a reorder hold go.
        void emit(std::span<const std::byte> frame);
        /// Delivers the reorder hold if there is one.
        void release_hold();
        /// A uniform double in [0, 1) from the engine's raw output. The caller holds `impairment_mutex_`.
        [[nodiscard]] double draw() noexcept;

        void fill_checksums(Packet& packet) const noexcept;
        [[nodiscard]] device::RxMetadata inspect(std::span<const std::byte> frame, std::uint16_t& queue) const noexcept;

        Fabric& fabric_;
        PortConfig config_;
        device::Capabilities capabilities_;
        device::RssDescription steering_;
        std::vector<std::unique_ptr<Queue>> queues_;
        Impairment impairment_;
        std::mt19937_64 engine_{impairment_.seed};
        ImpairmentCounters impairment_counters_;
        std::optional<std::vector<std::byte>> hold_;
        std::uint64_t egress_index_ = 0;
        mutable std::mutex impairment_mutex_;
    };

    /**
     * @brief An in-memory broadcast domain of ports: the fixture the layers above are tested,
     * simulated and demonstrated on. Never a production path.
     *
     * Transmit delivers at once: unicast to the port with that MAC (the sender included), broadcast
     * and multicast to every other port, unless the source port's impairment drops, duplicates, holds or
     * delays the frame. Add every port before any thread transmits.
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

        /// The fixture's clock step: delivers the delayed frames that are due in (due, sequence) order, releases the
        /// reorder holds, then marks every OnStep loan transmitted before the call finished. Safe from any thread. An
        /// earlier stamp than the last changes nothing.
        void advance(core::TimePoint now);

        [[nodiscard]] core::TimePoint now() const noexcept;

    private:
        friend class Port;

        struct DelayedFrame {
            core::TimePoint due;
            std::uint64_t sequence = 0;
            Port* source           = nullptr;
            std::vector<std::byte> bytes;
        };

        void deliver(const Port& source, std::span<const std::byte> frame);
        /// Queues the frame to be delivered by the first `advance` at or past `now_ + by`. Takes `mutex_` only to push.
        void delay(Port& source, std::span<const std::byte> frame, core::Duration by);

        std::vector<std::unique_ptr<Port>> ports_;
        mutable std::mutex mutex_;
        core::TimePoint now_{};
        std::vector<DelayedFrame> delayed_;
        std::uint64_t delayed_sequence_ = 0;
    };

}  // namespace aloe::fabric
