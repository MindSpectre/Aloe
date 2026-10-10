#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <ipv4_address.hpp>

namespace aloe::tcp {

    /// A connection as the table keys it: the received orientation, local address implied by the stack.
    struct FlowKey {
        wire::Ipv4Address remote{};
        std::uint16_t remote_port = 0;
        std::uint16_t local_port  = 0;

        friend constexpr bool operator==(const FlowKey&, const FlowKey&) noexcept = default;
    };

    namespace detail {

        /// The spec's 32-bit avalanche finalizer: applied to every base hash before a bucket is chosen.
        [[nodiscard]] constexpr std::uint32_t mix_flow_hash(std::uint32_t hash) noexcept {
            hash ^= hash >> 16U;
            hash *= 0x85ebca6bU;
            hash ^= hash >> 13U;
            hash *= 0xc2b2ae35U;
            hash ^= hash >> 16U;
            return hash;
        }

        /// FNV-1a over the key's eight network-order bytes: the base hash when the card's RSS hash is not usable.
        [[nodiscard]] constexpr std::uint32_t software_flow_hash(const FlowKey key) noexcept {
            std::uint32_t hash = 0x811c9dc5U;
            const auto mix     = [&hash](const std::uint8_t byte) {
                hash ^= byte;
                hash *= 0x01000193U;
            };
            for (const std::byte byte : key.remote.bytes()) {
                mix(std::to_integer<std::uint8_t>(byte));
            }
            mix(static_cast<std::uint8_t>(key.remote_port >> 8U));
            mix(static_cast<std::uint8_t>(key.remote_port & 0xffU));
            mix(static_cast<std::uint8_t>(key.local_port >> 8U));
            mix(static_cast<std::uint8_t>(key.local_port & 0xffU));
            return hash;
        }

    }  // namespace detail

    /**
     * @brief A fixed open-addressing index from a flow key to a connection slot.
     *
     * Capacity is the next power of two at or above twice `connections`, so the load never passes
     * one half. Linear probing, backward-shift deletion, no tombstones, no allocation after
     * construction. The caller supplies one base hash per key, computed the same way on insert,
     * find and erase; the table mixes it and masks it for the home bucket and stores the mixed
     * value so deletion can recover the home. Equality is on the key's fields, never on the hash.
     */
    class FlowTable {
    public:
        static constexpr std::uint32_t no_index = std::numeric_limits<std::uint32_t>::max();

        /// Throws std::invalid_argument for zero connections or a capacity that does not fit 32 bits.
        explicit FlowTable(std::size_t connections);

        [[nodiscard]] std::optional<std::uint32_t> find(FlowKey key, std::uint32_t base_hash) const noexcept;
        /// False when the key is present already or the table is full.
        [[nodiscard]] bool insert(FlowKey key, std::uint32_t index, std::uint32_t base_hash) noexcept;
        /// False when the key is absent.
        [[nodiscard]] bool erase(FlowKey key, std::uint32_t base_hash) noexcept;

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return entries_.size();
        }

    private:
        struct Entry {
            FlowKey key{};
            std::uint32_t index = no_index;
            std::uint32_t mixed = 0;
            bool used           = false;
        };

        [[nodiscard]] std::uint32_t home(const std::uint32_t mixed) const noexcept {
            return mixed & mask_;
        }

        /// How far `slot` is past `from`, cyclically.
        [[nodiscard]] std::uint32_t distance(const std::uint32_t from, const std::uint32_t slot) const noexcept {
            return (slot - from) & mask_;
        }

        [[nodiscard]] std::optional<std::uint32_t> locate(FlowKey key, std::uint32_t mixed) const noexcept;

        std::vector<Entry> entries_;
        std::uint32_t mask_ = 0;
        std::size_t size_   = 0;
    };

}  // namespace aloe::tcp
