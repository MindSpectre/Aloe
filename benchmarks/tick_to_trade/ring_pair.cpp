#include "ring_pair.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>

#include <rte_errno.h>
#include <rte_eth_ring.h>
#include <rte_lcore.h>
#include <rte_ring.h>

namespace aloe::benchmarks {

    namespace {

        /// `rte_eth_from_rings` registers the port as `net_ring_<name>`; the whole must fit a ring's name (28
        /// characters), so a port name is the config's name and two characters.
        constexpr std::string_view driver_prefix = "net_ring_";

        [[nodiscard]] rte_ring* create_ring(const std::string& name, const unsigned size) {
            rte_ring* ring =
                rte_ring_create(name.c_str(), size, static_cast<int>(rte_socket_id()), RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (ring == nullptr) {
                throw std::runtime_error{std::format("rte_ring_create({}): {}", name, rte_strerror(rte_errno))};
            }
            return ring;
        }

        void
        create_port(const std::string& name, const std::span<rte_ring* const> rx, const std::span<rte_ring* const> tx) {
            const int port = rte_eth_from_rings(name.c_str(),
                                                rx.data(),
                                                static_cast<unsigned>(rx.size()),
                                                tx.data(),
                                                static_cast<unsigned>(tx.size()),
                                                rte_socket_id());
            if (port < 0) {
                throw std::runtime_error{std::format("rte_eth_from_rings({}): {}", name, rte_strerror(rte_errno))};
            }
        }

    }  // namespace

    RingPair::RingPair(const RingPairConfig& config) {
        try {
            for (std::uint16_t queue = 0; queue < config.queues; ++queue) {
                to_stack_.push_back(create_ring(std::format("{}-to-stack-{}", config.name, queue), config.ring_size));
                to_peer_.push_back(create_ring(std::format("{}-to-peer-{}", config.name, queue), config.ring_size));
            }
            create_port(config.name + "-s", to_stack_, to_peer_);
            create_port(config.name + "-p", to_peer_, to_stack_);
            stack_ = std::make_unique<ethdev::Port>(
                ethdev::PortConfig{.name        = std::string{driver_prefix} + config.name + "-s",
                                   .queues      = config.queues,
                                   .descriptors = config.descriptors,
                                   .pool_size   = config.pool_size});
            peer_ = std::make_unique<ethdev::Port>(
                ethdev::PortConfig{.name        = std::string{driver_prefix} + config.name + "-p",
                                   .queues      = config.queues,
                                   .descriptors = config.descriptors,
                                   .pool_size   = config.pool_size});
        } catch (...) {
            release();
            throw;
        }
    }

    RingPair::~RingPair() {
        if (stack_ && peer_) {
            drain();
        }
        release();
    }

    void RingPair::drain() noexcept {
        std::array<ethdev::Packet, ethdev::Port::max_burst> burst{};
        for (ethdev::Port* port : {stack_.get(), peer_.get()}) {
            for (std::uint16_t queue = 0; queue < port->queue_count(); ++queue) {
                for (std::size_t got = port->receive(queue, burst); got > 0; got = port->receive(queue, burst)) {
                    for (ethdev::Packet& packet : std::span{burst}.first(got)) {
                        packet = ethdev::Packet{};
                    }
                }
            }
        }
    }

    void RingPair::release() noexcept {
        stack_.reset();  // closes the port; an attached ring port does not free its rings
        peer_.reset();
        for (rte_ring* ring : to_stack_) {
            rte_ring_free(ring);
        }
        for (rte_ring* ring : to_peer_) {
            rte_ring_free(ring);
        }
        to_stack_.clear();
        to_peer_.clear();
    }

}  // namespace aloe::benchmarks
