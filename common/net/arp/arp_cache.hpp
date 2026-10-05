#pragma once

#include <aloe/core>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <address.hpp>

namespace aloe::net {

    struct ArpCacheConfig {
        std::size_t capacity     = 256;  ///< Slots; a power of two of at least `ArpCache::probe_window`.
        core::Duration reachable = std::chrono::seconds{60};   ///< A confirmed MAC is used without a refresh this long.
        core::Duration expire    = std::chrono::seconds{120};  ///< After this, the address is unresolved again.
        core::Duration request_interval = std::chrono::seconds{1};  ///< At most one request per entry per interval.
    };

    /**
     * @brief The per-shard table of IPv4 address to MAC, aged lazily against the stamp the caller passes.
     *
     * A fixed table: open addressing with linear probing bounded to `probe_window` slots from a
     * multiplicative hash of the address. A slot is free or occupied and never goes back to free, so
     * probe chains stay intact without tombstones. A new entry takes the first free slot in its
     * window, else the first expired one, else the one with the oldest confirmation. One thread uses
     * a cache; nothing in it is synchronised, and nothing allocates after construction.
     *
     * An entry is incomplete (asked about, not answered) or reachable (confirmed at some stamp).
     * `lookup` returns the MAC while the confirmation is younger than `expire`, asks for a request
     * once it is `reachable` old, and asks for one for an incomplete or absent address, at most one
     * per `request_interval` per entry.
     */
    // TODO: Issue#5 - research replacing this table with abseil's flat_hash_set once #6's connection
    // table decides whether abseil enters the project: reserved capacity, inserts capped at it.
    class ArpCache {
    public:
        static constexpr std::size_t probe_window = 8;

        struct Lookup {
            std::optional<device::MacAddress> mac = std::nullopt;  ///< Present while the entry is reachable or stale.
            bool send_request                     = false;  ///< A request is due now; its stamp has been recorded.

            friend constexpr bool operator==(const Lookup&, const Lookup&) noexcept = default;
        };

        /// Throws std::invalid_argument for a capacity that is not a power of two of at least `probe_window`,
        /// a non-positive duration, or `reachable` not below `expire`.
        explicit ArpCache(const ArpCacheConfig& config);
        ArpCache(const ArpCache&)            = delete;
        ArpCache& operator=(const ArpCache&) = delete;
        ArpCache(ArpCache&&)                 = delete;
        ArpCache& operator=(ArpCache&&)      = delete;
        ~ArpCache()                          = default;

        /// Inserts an incomplete entry for an absent address.
        [[nodiscard]] Lookup lookup(device::Ipv4Address address, core::TimePoint now) noexcept;
        /// Sets the entry reachable at `now`, inserting it if absent.
        void learn(device::Ipv4Address address, device::MacAddress mac, core::TimePoint now) noexcept;
        /// Any entry for the address, incomplete or expired included.
        [[nodiscard]] bool contains(device::Ipv4Address address) const noexcept;

        [[nodiscard]] std::size_t size() const noexcept {
            return occupied_;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return slots_.size();
        }

        [[nodiscard]] const ArpCacheConfig& config() const noexcept {
            return config_;
        }

    private:
        enum class State : std::uint8_t { Free, Incomplete, Reachable };

        struct Entry {
            device::Ipv4Address address{};
            device::MacAddress mac{};
            State state = State::Free;
            core::TimePoint
                confirmed{};  ///< Meaningful when reachable; the epoch otherwise, so incomplete entries are the oldest.
            std::optional<core::TimePoint> requested = std::nullopt;  ///< The last request; none yet when absent.
        };

        [[nodiscard]] std::size_t home(device::Ipv4Address address) const noexcept;
        [[nodiscard]] const Entry* find(device::Ipv4Address address) const noexcept;
        [[nodiscard]] Entry* find(device::Ipv4Address address) noexcept;
        [[nodiscard]] Entry& insert(device::Ipv4Address address, core::TimePoint now) noexcept;
        [[nodiscard]] bool expired(const Entry& entry, core::TimePoint now) const noexcept;
        /// True, and records the stamp, when the entry's last request is absent or at least an interval old.
        [[nodiscard]] bool request_due(Entry& entry, core::TimePoint now) const noexcept;

        ArpCacheConfig config_;
        std::vector<Entry> slots_;
        unsigned shift_;  ///< 32 minus log2(capacity): the high bits of the hash product pick the home slot.
        std::size_t occupied_ = 0;
    };

}  // namespace aloe::net
