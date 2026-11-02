#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <flow_table.hpp>
#include <ipv4_address.hpp>
#include <ipv4_protocol.hpp>
#include <rss.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /// Where a `connect` lands: the local port it takes and the flow's base hash.
    struct Placement {
        std::uint16_t port      = 0;
        std::uint32_t base_hash = 0;
    };

    /**
     * @brief The shard's directory of connections: which slots are free, which slot each flow is in, which ports
     * listen, and which ephemeral port a connect takes so that the card steers the replies to this queue.
     *
     * The slots themselves are the stack's; the table hands their indices out and takes them back, slot 0 first. When
     * the card hashes IPv4 TCP tuples, a flow's base hash is the card's, so the table agrees with the card's steering;
     * otherwise it is a software hash of the key. Everything is allocated at construction.
     */
    class ConnectionTable {
    public:
        ConnectionTable(const TcpConfig& config, const device::RssDescription& steering)
            : flows_{config.connections},
              listener_limit_{config.listeners},
              first_port_{config.ephemeral_first},
              last_port_{config.ephemeral_last},
              cursor_{config.ephemeral_first},
              hardware_hash_{steering.enabled && !steering.table.empty() && steering.types.ipv4_tcp} {
            listeners_.reserve(config.listeners);
            free_.reserve(config.connections);
            for (std::size_t index = config.connections; index > 0; --index) {
                free_.push_back(static_cast<std::uint32_t>(index - 1));  // slot 0 is taken first
            }
        }

        [[nodiscard]] std::optional<std::uint32_t> take_slot() noexcept {
            if (free_.empty()) {
                return std::nullopt;
            }
            const std::uint32_t index = free_.back();
            free_.pop_back();
            return index;
        }

        /// A slot taken and now free; within the reserved capacity, so no allocation.
        void free_slot(const std::uint32_t index) noexcept {
            free_.push_back(index);
        }

        [[nodiscard]] std::optional<std::uint32_t> find(const FlowKey key,
                                                        const std::uint32_t base_hash) const noexcept {
            return flows_.find(key, base_hash);
        }

        [[nodiscard]] bool insert(const FlowKey key, const std::uint32_t slot, const std::uint32_t base_hash) noexcept {
            return flows_.insert(key, slot, base_hash);
        }

        [[nodiscard]] bool erase(const FlowKey key, const std::uint32_t base_hash) noexcept {
            return flows_.erase(key, base_hash);
        }

        /// Flows indexed.
        [[nodiscard]] std::size_t size() const noexcept {
            return flows_.size();
        }

        [[nodiscard]] std::expected<void, ListenError> listen(const std::uint16_t port) noexcept {
            if (listening(port)) {
                return std::unexpected{ListenError::InUse};
            }
            if (listeners_.size() == listener_limit_) {
                return std::unexpected{ListenError::TableFull};
            }
            listeners_.push_back(port);  // within the reserved capacity: no allocation
            return {};
        }

        void unlisten(const std::uint16_t port) noexcept {
            if (const auto found = std::ranges::find(listeners_, port); found != listeners_.end()) {
                *found = listeners_.back();
                listeners_.pop_back();
            }
        }

        [[nodiscard]] bool listening(const std::uint16_t port) const noexcept {
            return std::ranges::find(listeners_, port) != listeners_.end();
        }

        /// The base hash of a received segment's flow: the card's RSS hash when it hashes TCP tuples, computed from
        /// `steering` when the packet did not carry it, and the software hash of the key otherwise.
        [[nodiscard]] std::uint32_t base_hash(const device::RssDescription& steering,
                                              const device::FlowTuple& received,
                                              const std::optional<std::uint32_t> card_hash) const noexcept {
            if (!hardware_hash_) {
                return software_flow_hash(FlowKey{.remote      = received.source,
                                                  .remote_port = received.source_port,
                                                  .local_port  = received.destination_port});
            }
            return card_hash ? *card_hash : device::flow_hash(steering, received);
        }

        /**
         * The next ephemeral port, from a cursor that wraps over the range, whose flow to `peer` is free, is no
         * listener's, and lands on `queue` under `steering`. With the card steering on addresses only, or not at all,
         * the queue does not depend on the port: a peer this queue does not receive from is `Unplaceable` whatever
         * the port. `NoPort` when the whole range is taken.
         */
        [[nodiscard]] std::expected<Placement, ConnectError> place(const Endpoint peer,
                                                                   const wire::Ipv4Address local,
                                                                   const device::RssDescription& steering,
                                                                   const std::uint16_t queue) noexcept {
            device::FlowTuple tuple{.source           = peer.address,  // received orientation: the peer is the source
                                    .destination      = local,
                                    .source_port      = peer.port,
                                    .destination_port = 0,
                                    .protocol         = wire::Ipv4Protocol::Tcp};
            if (!hardware_hash_ && device::queue_for(steering, tuple) != queue) {
                return std::unexpected{ConnectError::Unplaceable};
            }
            const std::uint32_t range = static_cast<std::uint32_t>(last_port_ - first_port_) + 1U;
            for (std::uint32_t tried = 0; tried < range; ++tried) {
                const std::uint16_t port = cursor_;
                cursor_ = cursor_ == last_port_ ? first_port_ : static_cast<std::uint16_t>(cursor_ + 1);
                if (listening(port)) {
                    continue;
                }
                tuple.destination_port = port;
                if (hardware_hash_ && device::queue_for(steering, tuple) != queue) {
                    continue;
                }
                const FlowKey key{.remote = peer.address, .remote_port = peer.port, .local_port = port};
                const std::uint32_t hash =
                    hardware_hash_ ? device::flow_hash(steering, tuple) : software_flow_hash(key);
                if (flows_.find(key, hash)) {
                    continue;
                }
                return Placement{.port = port, .base_hash = hash};
            }
            return std::unexpected{ConnectError::NoPort};
        }

    private:
        FlowTable flows_;
        std::vector<std::uint16_t> listeners_ = {};
        std::size_t listener_limit_           = 0;
        std::vector<std::uint32_t> free_      = {};  ///< A stack of free slot indices.
        std::uint16_t first_port_             = 0;
        std::uint16_t last_port_              = 0;
        std::uint16_t cursor_                 = 0;
        bool hardware_hash_ = false;  ///< The card hashes IPv4 TCP tuples: its hash is the table's base hash.
    };

}  // namespace aloe::tcp::detail
