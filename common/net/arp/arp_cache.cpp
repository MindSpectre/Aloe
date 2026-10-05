#include "arp_cache.hpp"

#include <bit>
#include <stdexcept>
#include <utility>

namespace aloe::net {

    namespace {

        /// 2^32 / phi, odd: every input bit moves the high output bits, which `home` keeps.
        constexpr std::uint32_t golden_ratio = 0x9e3779b1U;
        constexpr unsigned address_bits      = 32;

        const ArpCacheConfig& validated(const ArpCacheConfig& config) {
            if (config.capacity < ArpCache::probe_window || !std::has_single_bit(config.capacity)) {
                throw std::invalid_argument{"ArpCacheConfig::capacity must be a power of two of at least 8"};
            }
            if (config.reachable <= core::Duration::zero() || config.request_interval <= core::Duration::zero()) {
                throw std::invalid_argument{"ArpCacheConfig durations must be positive"};
            }
            if (config.reachable >= config.expire) {
                throw std::invalid_argument{"ArpCacheConfig::reachable must be below expire"};
            }
            return config;
        }

        void reset(auto& entry, const device::Ipv4Address address) noexcept {
            entry.address   = address;
            entry.mac       = {};
            entry.confirmed = {};
            entry.requested.reset();
        }

    }  // namespace

    ArpCache::ArpCache(const ArpCacheConfig& config)
        : config_{validated(config)},
          slots_(config.capacity),
          shift_{address_bits - static_cast<unsigned>(std::countr_zero(config.capacity))} {
    }

    std::size_t ArpCache::home(const device::Ipv4Address address) const noexcept {
        return static_cast<std::size_t>((address.to_uint32() * golden_ratio) >> shift_);
    }

    const ArpCache::Entry* ArpCache::find(const device::Ipv4Address address) const noexcept {
        const std::size_t mask  = slots_.size() - 1;
        const std::size_t start = home(address);
        for (std::size_t probe = 0; probe < probe_window; ++probe) {
            const Entry& slot = slots_[(start + probe) & mask];
            if (slot.state == State::Free) {
                return nullptr;  // chains never contain a free slot, so the address is not here
            }
            if (slot.address == address) {
                return &slot;
            }
        }
        return nullptr;
    }

    ArpCache::Entry* ArpCache::find(const device::Ipv4Address address) noexcept {
        return const_cast<Entry*>(std::as_const(*this).find(address));  // one search, two constnesses
    }

    bool ArpCache::expired(const Entry& entry, const core::TimePoint now) const noexcept {
        switch (entry.state) {
            case State::Free:
                return true;
            case State::Incomplete:
                return entry.requested.has_value() && now - *entry.requested >= config_.expire;
            case State::Reachable:
                return now - entry.confirmed >= config_.expire;
        }
        std::unreachable();
    }

    ArpCache::Entry& ArpCache::insert(const device::Ipv4Address address, const core::TimePoint now) noexcept {
        const std::size_t mask  = slots_.size() - 1;
        const std::size_t start = home(address);
        Entry* first_expired    = nullptr;
        Entry* oldest           = nullptr;
        for (std::size_t probe = 0; probe < probe_window; ++probe) {
            Entry& slot = slots_[(start + probe) & mask];
            if (slot.state == State::Free) {
                ++occupied_;
                reset(slot, address);
                slot.state = State::Incomplete;
                return slot;
            }
            if (first_expired == nullptr && expired(slot, now)) {
                first_expired = &slot;
            }
            if (oldest == nullptr || slot.confirmed < oldest->confirmed) {
                oldest = &slot;
            }
        }
        Entry& victim = first_expired != nullptr ? *first_expired : *oldest;
        reset(victim, address);
        victim.state = State::Incomplete;
        return victim;
    }

    bool ArpCache::request_due(Entry& entry, const core::TimePoint now) const noexcept {
        if (entry.requested.has_value() && now - *entry.requested < config_.request_interval) {
            return false;
        }
        entry.requested = now;
        return true;
    }

    ArpCache::Lookup ArpCache::lookup(const device::Ipv4Address address, const core::TimePoint now) noexcept {
        Entry* entry = find(address);
        if (entry == nullptr) {
            entry = &insert(address, now);
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
        return {.mac = std::nullopt, .send_request = request_due(*entry, now)};
    }

    void ArpCache::learn(const device::Ipv4Address address,
                         const device::MacAddress mac,
                         const core::TimePoint now) noexcept {
        Entry* entry = find(address);
        if (entry == nullptr) {
            entry = &insert(address, now);
        }
        entry->mac       = mac;
        entry->state     = State::Reachable;
        entry->confirmed = now;
        entry->requested.reset();
    }

    bool ArpCache::contains(const device::Ipv4Address address) const noexcept {
        return find(address) != nullptr;
    }

}  // namespace aloe::net
