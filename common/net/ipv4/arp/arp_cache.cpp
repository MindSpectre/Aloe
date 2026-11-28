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
            // A stamp plus a lifetime, or plus the request interval (`Output::prebuild`'s `valid_until`), cannot
            // overflow, as TcpConfig bounds its own.
            if (config.reachable > core::Duration::max() / 4) {
                throw std::invalid_argument{"ArpCacheConfig::reachable must fit a quarter of core::Duration"};
            }
            if (config.expire > core::Duration::max() / 4) {
                throw std::invalid_argument{"ArpCacheConfig::expire must fit a quarter of core::Duration"};
            }
            if (config.request_interval > core::Duration::max() / 4) {
                throw std::invalid_argument{"ArpCacheConfig::request_interval must fit a quarter of core::Duration"};
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

    bool ArpCache::lapsed(const Entry& entry, const core::TimePoint now) const noexcept {
        return entry.state == State::Incomplete &&
               (!entry.requested.has_value() || now - *entry.requested >= config_.request_interval);
    }

    bool ArpCache::expired(const Entry& entry, const core::TimePoint now) const noexcept {
        return entry.state == State::Reachable && now - entry.confirmed >= config_.expire;
    }

    ArpCache::Entry* ArpCache::insert(const wire::Ipv4Address address, const core::TimePoint now) noexcept {
        if (held_ == config_.capacity) {
            // Linux's neigh_alloc makes room by a forced collection (neigh_forced_gc); here a full table refuses, and
            // only a slot no mapping needs is taken: a lapsed one first, whose leaving takes no MAC and moves no
            // generation, else an expired one. The scan reads every slot, and only a new address at a full table makes
            // it.
            Entry* taken = nullptr;
            for (Entry& entry : slots_) {
                if (lapsed(entry, now)) {
                    taken = &entry;
                    break;
                }
                if (taken == nullptr && expired(entry, now)) {
                    taken = &entry;
                }
            }
            if (taken == nullptr) {
                return nullptr;
            }
            if (taken->state == State::Reachable) {
                ++generation_;  // its MAC goes: an Ipv4Prebuilt of this generation may still name it
            }
            remove(static_cast<std::size_t>(taken - slots_.data()));
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

    std::optional<ArpCache::Taught> ArpCache::kept_out(const wire::Ipv4Address address,
                                                       const wire::MacAddress mac) const noexcept {
        if (const ArpResolution* known = find_static(address); known != nullptr) {
            return known->mac == mac ? Taught::Static : Taught::Conflict;  // nothing changes the static table
        }
        if (!config_.learning) {
            return Taught::Off;
        }
        return std::nullopt;
    }

    ArpCache::Taught
    ArpCache::update(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
        if (const std::optional<Taught> refused = kept_out(address, mac); refused.has_value()) {
            return *refused;
        }
        Entry* entry = find(address);
        if (entry == nullptr) {
            return Taught::Unknown;
        }
        // Only an answer to a request of the brick's own extends the entry's life: an incomplete entry's, or the
        // refresh a stale entry's lookup asked for. A frame nobody asked for may give a resolved entry another MAC,
        // but never moves its confirmation, so frames that keep naming an address cannot keep it in the table. Linux's
        // `arp_process` (net_ipv4_arp.c:933-945) lets a request or a broadcast reply assert nothing either: it updates
        // the entry NUD_STALE, and `__neigh_update` moves `confirmed` only for a connected state. The test here is
        // whether the brick asked, not which frame answered, so it departs from Linux's unicast reply both ways. It is
        // narrower in when: a unicast reply nobody asked for confirms nothing here, where Linux takes it as
        // reachable. It is wider in which frame: while a request is outstanding a request or a broadcast reply
        // confirms too, which Linux leaves stale (net_ipv4_arp.c:938-943).
        if (entry->state == State::Incomplete || entry->requested.has_value()) {
            confirm(*entry, mac, now);
        } else if (entry->mac != mac) {
            entry->mac = mac;
            ++generation_;
        }
        return Taught::Updated;
    }

    ArpCache::Taught
    ArpCache::seed(const wire::Ipv4Address address, const wire::MacAddress mac, const core::TimePoint now) noexcept {
        if (const std::optional<Taught> refused = kept_out(address, mac); refused.has_value()) {
            return *refused;
        }
        Taught taught = Taught::Updated;
        Entry* entry  = find(address);
        if (entry == nullptr) {
            entry = insert(address, now);
            if (entry == nullptr) {
                return Taught::Full;
            }
            taught = Taught::Created;
        }
        confirm(*entry, mac, now);  // the program's own word: it confirms whatever the entry was
        return taught;
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
