#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <arp_resolution.hpp>

namespace aloe::net {

    struct ArpCacheConfig {
        std::size_t capacity            = 256;  ///< The learned entries held at most: positive, and at most 2^30.
        /// A confirmed MAC is used without a refresh this long; at most a quarter of `core::Duration`.
        core::Duration reachable        = std::chrono::seconds{60};
        /// After this, the address is unresolved again; at most a quarter of `core::Duration`.
        core::Duration expire           = std::chrono::seconds{120};
        /// At most one request per entry per interval; at most a quarter of `core::Duration`.
        core::Duration request_interval = std::chrono::seconds{1};
        /// Resolve on the wire and keep what is learned. Off, the learned table is neither allocated nor read, the
        /// settings above are not checked, and only the static entries resolve. The brick passes
        /// `Ipv4Config::arp_learning`; both are off by default.
        bool learning                   = false;
    };

    namespace detail {

        /// The home slot of `address` in a learned table of `slots` slots, a power of two from 2 to 2^31: the high bits
        /// of a multiplicative hash by 2^32 / phi, odd, so every bit of the address moves them.
        [[nodiscard]] constexpr std::size_t arp_home_slot(const wire::Ipv4Address address,
                                                          const std::size_t slots) noexcept {
            constexpr std::uint32_t golden_ratio = 0x9e3779b1U;
            constexpr unsigned address_bits      = 32;
            const auto shift                     = address_bits - static_cast<unsigned>(std::countr_zero(slots));
            return static_cast<std::size_t>(static_cast<std::uint32_t>(address.to_uint32() * golden_ratio) >> shift);
        }

    }  // namespace detail

    /**
     * @brief The per-shard table of IPv4 address to MAC: the static entries, then what is learned on the wire, aged
     * lazily against the stamp the caller passes.
     *
     * The static entries come from the configuration and are frozen for the cache's life: an array searched first, in
     * a row, since a handful of addresses are compared faster than any table is hashed. A static entry answers every
     * lookup with its MAC, asks for no request, never ages and never moves the generation; `seed` and `update` leave
     * it alone. With learning off nothing else is kept: a lookup of any other address answers nothing and asks for no
     * request.
     *
     * The learned table is fixed: open addressing with linear probing and backward-shift deletion, as
     * `tcp::FlowTable`, over the next power of two at or above twice `capacity` slots, so it is never more than half
     * full and any free slot serves any address. Only the brick's own `lookup` and the program's `seed` create an
     * entry; a frame calls `update`, which changes an entry the table holds and creates none. No address takes
     * another's place: a new one takes a free slot while fewer than `capacity` entries are held, and then only the
     * slot of an incomplete one whose last request is at least `request_interval` old, else of an expired entry. A
     * resolved entry that has not expired is never removed; with no such slot the table refuses the address. One
     * thread uses a cache; nothing in it is synchronised, and nothing allocates after construction.
     *
     * An entry is incomplete (asked about, not answered) or reachable (confirmed at some stamp).
     * `lookup` returns the MAC while the confirmation is younger than `expire`, asks for a request
     * once it is `reachable` old, and asks for one for an incomplete or absent address, at most one
     * per `request_interval` per entry. Only an answer to such a request, or a `seed`, confirms an entry: a frame
     * nobody asked for may give a resolved entry another MAC, but never extends its life, so an entry the brick does
     * not use expires `expire` after the last answer to its own request, whatever frames name it.
     */
    class ArpCache {
    public:
        struct Lookup {
            std::optional<wire::MacAddress> mac = std::nullopt;  ///< Present for a static, reachable or stale entry.
            bool send_request                   = false;         ///< A request is due now; its stamp has been recorded.
            bool permanent = false;  ///< A static entry: it never ages, and a template built from it never goes stale.
            bool full      = false;  ///< The address is absent and the table had no slot to give it.

            friend constexpr bool operator==(const Lookup&, const Lookup&) noexcept = default;
        };

        /// What a frame's or the program's mapping did to the table.
        enum class Taught : std::uint8_t {
            Updated,   ///< A learned entry was filled, refreshed or given another MAC, or named with the MAC it has.
            Created,   ///< `seed` made a new entry.
            Unknown,   ///< `update`: the table holds no entry for the address; nothing was stored.
            Static,    ///< The address is static and the MAC agrees; nothing changed.
            Conflict,  ///< The address is static and the MAC differs; nothing changed.
            Full,      ///< `seed`: no free slot, no expired entry and no lapsed request; nothing was stored.
            Off,       ///< Learning is off; nothing was stored.
        };

        /// With learning on, throws std::invalid_argument for a capacity that is zero or above 2^30, a non-positive
        /// duration, a duration above a quarter of `core::Duration`, or `reachable` not below `expire`.
        /// `static_entries` are taken as the brick validated them
        /// (`Ipv4Config::static_arp`): unicast neighbours, each listed once, at unicast MACs.
        explicit ArpCache(const ArpCacheConfig& config, std::vector<ArpResolution> static_entries = {});
        ArpCache(const ArpCache&)            = delete;
        ArpCache& operator=(const ArpCache&) = delete;
        ArpCache(ArpCache&&)                 = delete;
        ArpCache& operator=(ArpCache&&)      = delete;
        ~ArpCache()                          = default;

        /// A static entry's MAC, permanent; with learning off nothing else. With learning on, inserts an incomplete
        /// entry for an absent address, or answers `full` and asks for no request when the table has no slot for it.
        [[nodiscard]] Lookup lookup(wire::Ipv4Address address, core::TimePoint now) noexcept;
        /// A frame's mapping: fills, refreshes or changes the entry the table holds for `address`, and creates none. It
        /// confirms the entry at `now` only when it answers a request the brick has outstanding for the address, the
        /// incomplete entry's or a stale one's refresh; otherwise it gives a resolved entry another MAC, or changes
        /// nothing, and leaves its confirmation where it was.
        [[nodiscard]] Taught update(wire::Ipv4Address address, wire::MacAddress mac, core::TimePoint now) noexcept;
        /// The program's mapping: confirms the entry the table holds for `address` at `now` with `mac`, whatever it
        /// was, and creates the entry when the table does not hold it.
        [[nodiscard]] Taught seed(wire::Ipv4Address address, wire::MacAddress mac, core::TimePoint now) noexcept;
        /// Any learned entry for the address, incomplete or expired included; a static one is not counted.
        [[nodiscard]] bool contains(wire::Ipv4Address address) const noexcept;
        /// The confirmation stamp of a reachable learned entry, expired or not; nothing for an absent, incomplete or
        /// static one.
        [[nodiscard]] std::optional<core::TimePoint> confirmed(wire::Ipv4Address address) const noexcept;
        /// Whether the address has a static entry.
        [[nodiscard]] bool is_static(wire::Ipv4Address address) const noexcept;

        /// Starts at zero; moves by one whenever a learned entry's MAC changes or goes: an `update` or `seed` that
        /// fills an incomplete entry or gives an entry another MAC, a `lookup` that finds a reachable entry expired,
        /// and a new address that takes the slot of an expired reachable entry. The static entries never move it.
        [[nodiscard]] std::uint64_t generation() const noexcept {
            return generation_;
        }

        /// The learned entries held.
        [[nodiscard]] std::size_t size() const noexcept {
            return held_;
        }

        /// The learned entries the table holds at most: none with learning off.
        [[nodiscard]] std::size_t capacity() const noexcept {
            return config_.learning ? config_.capacity : 0;
        }

        [[nodiscard]] const ArpCacheConfig& config() const noexcept {
            return config_;
        }

    private:
        enum class State : std::uint8_t { Free, Incomplete, Reachable };

        struct Entry {
            wire::Ipv4Address address{};
            wire::MacAddress mac{};
            State state = State::Free;
            core::TimePoint confirmed{};  ///< Meaningful when reachable; the epoch otherwise.
            std::optional<core::TimePoint> requested = std::nullopt;  ///< The last request; none yet when absent.
        };

        [[nodiscard]] const ArpResolution* find_static(wire::Ipv4Address address) const noexcept;
        [[nodiscard]] std::size_t home(wire::Ipv4Address address) const noexcept;
        [[nodiscard]] const Entry* find(wire::Ipv4Address address) const noexcept;
        [[nodiscard]] Entry* find(wire::Ipv4Address address) noexcept;
        /// An incomplete entry for the absent `address`, in a free slot or in the slot of a reusable entry; nothing
        /// when the table is full and no entry is reusable.
        [[nodiscard]] Entry* insert(wire::Ipv4Address address, core::TimePoint now) noexcept;
        /// Empties `slot` and moves the entries behind it back along their probe chains, so no chain has a gap.
        void remove(std::size_t slot) noexcept;
        /// Incomplete, with no request younger than `request_interval`: no reply is coming, and its slot may be taken.
        [[nodiscard]] bool lapsed(const Entry& entry, core::TimePoint now) const noexcept;
        /// Reachable, and confirmed at least `expire` ago: its slot may be taken, and its MAC goes with it.
        [[nodiscard]] bool expired(const Entry& entry, core::TimePoint now) const noexcept;
        /// `Static` or `Conflict` for a static address, `Off` with learning off: the learned table may not take the
        /// mapping. Nothing when it may.
        [[nodiscard]] std::optional<Taught> kept_out(wire::Ipv4Address address, wire::MacAddress mac) const noexcept;
        /// True, and records the stamp, when the entry's last request is absent or at least an interval old.
        [[nodiscard]] bool request_due(Entry& entry, core::TimePoint now) const noexcept;
        /// Sets the entry reachable at `now` with `mac`, moving the generation when its MAC appears or changes.
        void confirm(Entry& entry, wire::MacAddress mac, core::TimePoint now) noexcept;

        ArpCacheConfig config_;
        std::vector<ArpResolution> static_;  ///< Sized and filled at construction, never changed after.
        std::vector<Entry> slots_;  ///< Twice `capacity` rounded up to a power of two; empty with learning off.
        std::size_t held_         = 0;
        std::uint64_t generation_ = 0;
    };

}  // namespace aloe::net
