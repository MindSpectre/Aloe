#include "arp_cache.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace aloe::net {

    namespace {

        /// Twice this many slots still fit the 32-bit hash `detail::arp_home_slot` takes the home from.
        constexpr std::size_t max_capacity = std::size_t{1} << 30U;

        const ArpCacheConfig& validated(const ArpCacheConfig& config) {
            if (!config.learning) {
                return config;  // the learned table's settings are never read
            }
            if (config.capacity == 0 || config.capacity > max_capacity) {
                throw std::invalid_argument{"ArpCacheConfig::capacity must be positive and at most 2^30"};
            }
            if (config.reachable <= core::Duration::zero() || config.request_interval <= core::Duration::zero()) {
                throw std::invalid_argument{"ArpCacheConfig durations must be positive"};
            }
            if (config.reachable >= config.expire) {
                throw std::invalid_argument{"ArpCacheConfig::reachable must be below expire"};
            }
            return config;
        }

    }  // namespace

    ArpCache::ArpCache(const ArpCacheConfig& config, std::vector<ArpResolution> static_entries)
        : config_{validated(config)},
          static_{std::move(static_entries)},
          slots_(config.learning ? std::bit_ceil(2 * config.capacity) : 0) {
    }

    const ArpResolution* ArpCache::find_static(const wire::Ipv4Address address) const noexcept {
        const auto found = std::ranges::find(static_, address, &ArpResolution::address);
        return found != static_.end() ? &*found : nullptr;
    }

    std::size_t ArpCache::home(const wire::Ipv4Address address) const noexcept {
        return detail::arp_home_slot(address, slots_.size());
    }

    const ArpCache::Entry* ArpCache::find(const wire::Ipv4Address address) const noexcept {
        if (slots_.empty()) {
            return nullptr;  // learning off: no learned table
        }
        const std::size_t mask = slots_.size() - 1;
        std::size_t slot       = home(address);
        // At most half the slots are held, so a chain always ends at a free slot; the count only bounds the loop.
        for (std::size_t probed = 0; probed < slots_.size(); ++probed) {
            const Entry& entry = slots_[slot];
            if (entry.state == State::Free) {
                return nullptr;
            }
            if (entry.address == address) {
                return &entry;
            }
            slot = (slot + 1) & mask;
        }
        return nullptr;
    }

    ArpCache::Entry* ArpCache::find(const wire::Ipv4Address address) noexcept {
        return const_cast<Entry*>(std::as_const(*this).find(address));  // one search, two constnesses
    }

    bool ArpCache::reusable(const Entry& entry, const core::TimePoint now) const noexcept {
        if (entry.state == State::Reachable) {
            return now - entry.confirmed >= config_.expire;
        }
        return !entry.requested.has_value() || now - *entry.requested >= config_.request_interval;
    }

    ArpCache::Entry* ArpCache::insert(const wire::Ipv4Address address, const core::TimePoint now) noexcept {
        if (held_ == config_.capacity) {
            // Linux's neigh_alloc makes room by a forced collection (neigh_forced_gc); here a full table refuses, and
            // only a slot no mapping needs is taken. The scan reads every slot, and only a new address at a full table
            // makes it.
            const auto taken = std::ranges::find_if(
                slots_, [this, now](const Entry& entry) { return entry.state != State::Free && reusable(entry, now); });
            if (taken == slots_.end()) {
                return nullptr;
            }
            if (taken->state == State::Reachable) {
                ++generation_;  // its MAC goes: an Ipv4Prebuilt of this generation may still name it
            }
            remove(static_cast<std::size_t>(taken - slots_.begin()));
        }
        const std::size_t mask = slots_.size() - 1;
        std::size_t slot       = home(address);
        while (slots_[slot].state != State::Free) {
            slot = (slot + 1) & mask;
        }
        slots_[slot] = Entry{.address = address, .state = State::Incomplete};
        ++held_;
        return &slots_[slot];
    }

    void ArpCache::remove(const std::size_t slot) noexcept {
        const std::size_t mask = slots_.size() - 1;
        const auto distance    = [mask](const std::size_t from, const std::size_t to) { return (to - from) & mask; };
        std::size_t hole       = slot;
        for (std::size_t next = (hole + 1) & mask; slots_[next].state != State::Free; next = (next + 1) & mask) {
            // An entry moves back into the hole only if the hole is not before its home on its probe path.
            if (const std::size_t its_home = home(slots_[next].address);
                distance(its_home, hole) < distance(its_home, next)) {
                slots_[hole] = slots_[next];
                hole         = next;
            }
        }
        slots_[hole] = Entry{};
        --held_;
    }

    bool ArpCache::request_due(Entry& entry, const core::TimePoint now) const noexcept {
        if (entry.requested.has_value() && now - *entry.requested < config_.request_interval) {
            return false;
        }
        entry.requested = now;
        return true;
    }

    void ArpCache::confirm(Entry& entry, const wire::MacAddress mac, const core::TimePoint now) noexcept {
        if (entry.state != State::Reachable || entry.mac != mac) {
            ++generation_;
        }
        entry.mac       = mac;
        entry.state     = State::Reachable;
        entry.confirmed = now;
        entry.requested.reset();
    }

    ArpCache::Lookup ArpCache::lookup(const wire::Ipv4Address address, const core::TimePoint now) noexcept {
        if (const ArpResolution* known = find_static(address); known != nullptr) {
            return {.mac = known->mac, .send_request = false, .permanent = true};
        }
        if (!config_.learning) {
            // Nothing is resolved on the wire unless the configuration switches learning on: where this departs from
            // RFC 1122 2.3.2, whose host resolves its neighbours with ARP.
            return {};
        }
        Entry* entry = find(address);
        if (entry == nullptr) {
            entry = insert(address, now);
            if (entry == nullptr) {
                return {.mac = std::nullopt, .send_request = false, .permanent = false, .full = true};
            }
        }
        if (entry->state == State::Incomplete) {
            return {.mac = std::nullopt, .send_request = request_due(*entry, now)};
        }
        const auto age = now - entry->confirmed;
        if (age < config_.reachable) {
            return {.mac = entry->mac, .send_request = false};
        }
        if (age < config_.expire) {
            return {.mac = entry->mac, .send_request = request_due(*entry, now)};
        }
        entry->state     = State::Incomplete;  // expiry observed: the MAC goes
        entry->mac       = {};
        entry->confirmed = {};
        ++generation_;
        return {.mac = std::nullopt, .send_request = request_due(*entry, now)};
    }

    ArpCache::Taught
    ArpCache::update(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
        if (const ArpResolution* known = find_static(address); known != nullptr) {
            return known->mac == mac ? Taught::Static : Taught::Conflict;  // nothing changes the static table
        }
        if (!config_.learning) {
            return Taught::Off;
        }
        Entry* entry = find(address);
        if (entry == nullptr) {
            return Taught::Unknown;
        }
        confirm(*entry, mac, now);
        return Taught::Updated;
    }

    ArpCache::Taught
    ArpCache::seed(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
        if (const Taught taught = update(address, mac, now); taught != Taught::Unknown) {
            return taught;
        }
        Entry* entry = insert(address, now);
        if (entry == nullptr) {
            return Taught::Full;
        }
        confirm(*entry, mac, now);
        return Taught::Created;
    }

    std::optional<core::TimePoint> ArpCache::confirmed(const wire::Ipv4Address address) const noexcept {
        const Entry* entry = find(address);
        if (entry == nullptr || entry->state != State::Reachable) {
            return std::nullopt;
        }
        return entry->confirmed;
    }

    bool ArpCache::contains(const wire::Ipv4Address address) const noexcept {
        return find(address) != nullptr;
    }

    bool ArpCache::is_static(const wire::Ipv4Address address) const noexcept {
        return find_static(address) != nullptr;
    }

}  // namespace aloe::net
