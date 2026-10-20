#pragma once

#include <aloe/ethdev>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct rte_ring;

namespace aloe::benchmarks {

    struct RingPairConfig {
        std::string name =
            "aloe-bench";  ///< Prefix of the ring and port names, at most 15 characters; unique in the process.
        std::uint16_t queues      = 1;
        unsigned ring_size        = 1024;  ///< Frames one direction holds; a power of two, as the ring driver's.
        std::uint32_t pool_size   = 4096;  ///< Mbufs per queue on each port.
        std::uint16_t descriptors = 1024;
    };

    /**
     * @brief Two Ethernet ports over cross-connected single-producer single-consumer rings.
     *
     * Per queue, `to_stack` carries what the peer transmits and the stack receives, and `to_peer` the reverse, so
     * nothing loops back onto its sender as a `net_ring` vdev's queue would. Both ports are built with
     * `rte_eth_from_rings` and wrapped in `ethdev::Port`, so the stack side is the `Port` a card gets: real mbufs,
     * software checksums, `max_burst` of 64. The rings outlive the ports and are freed last, after a drain.
     */
    class RingPair {
    public:
        explicit RingPair(const RingPairConfig& config);
        RingPair(const RingPair&)            = delete;
        RingPair& operator=(const RingPair&) = delete;
        RingPair(RingPair&&)                 = delete;
        RingPair& operator=(RingPair&&)      = delete;
        ~RingPair();

        [[nodiscard]] ethdev::Port& stack() noexcept {
            return *stack_;
        }

        [[nodiscard]] ethdev::Port& peer() noexcept {
            return *peer_;
        }

        /// Receives and frees every frame still on either direction of every queue, from the calling thread. Between
        /// configurations, with no stack alive, so the next stack never counts a stale frame as a drop.
        void drain() noexcept;

    private:
        void release() noexcept;

        std::vector<rte_ring*> to_stack_;
        std::vector<rte_ring*> to_peer_;
        std::unique_ptr<ethdev::Port> stack_;
        std::unique_ptr<ethdev::Port> peer_;
    };

}  // namespace aloe::benchmarks
