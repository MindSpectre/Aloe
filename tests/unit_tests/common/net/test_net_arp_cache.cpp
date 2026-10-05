#include <aloe/net>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;
    using Lookup    = aloe::net::ArpCache::Lookup;

    constexpr TimePoint start{};

    [[nodiscard]] TimePoint at(const std::chrono::nanoseconds offset) {
        return start + offset;
    }

    constexpr aloe::wire::Ipv4Address peer{10, 0, 0, 1};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress other_mac{0x02, 0, 0, 0, 0, 0x02};

    /// Eight slots and a window of eight: every address can reach every slot, so the ninth entry must evict.
    constexpr aloe::net::ArpCacheConfig small{.capacity = 8, .reachable = 60s, .expire = 120s, .request_interval = 1s};

    [[nodiscard]] aloe::wire::Ipv4Address host(const std::uint8_t index) {
        return {10, 0, 1, index};
    }

}  // namespace

TEST(ArpCache, AMissInsertsAnIncompleteEntryAndAsksForOneRequestPerInterval) {
    aloe::net::ArpCache cache{small};
    EXPECT_FALSE(cache.contains(peer));

    EXPECT_EQ(cache.lookup(peer, at(0s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_TRUE(cache.contains(peer));
    EXPECT_EQ(cache.size(), 1);

    EXPECT_EQ(cache.lookup(peer, at(500ms)), (Lookup{.mac = std::nullopt, .send_request = false}))
        << "one request per interval";
    EXPECT_EQ(cache.lookup(peer, at(1s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_EQ(cache.size(), 1) << "the same entry, not a second one";
}

TEST(ArpCache, LearnResolvesAndAReachableEntryAsksForNothing) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.lookup(peer, at(0s));
    cache.learn(peer, peer_mac, at(1ms));
    EXPECT_EQ(cache.lookup(peer, at(30s)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.lookup(peer, at(59s)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.size(), 1);
}

TEST(ArpCache, LearnWithoutALookupInsertsAReachableEntry) {
    aloe::net::ArpCache cache{small};
    cache.learn(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.lookup(peer, at(1s)), (Lookup{.mac = peer_mac, .send_request = false}));
}

TEST(ArpCache, AStaleEntryKeepsItsMacAndRefreshesOncePerInterval) {
    aloe::net::ArpCache cache{small};
    cache.learn(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.lookup(peer, at(60s)), (Lookup{.mac = peer_mac, .send_request = true}))
        << "stale: still usable, and a refresh is due";
    EXPECT_EQ(cache.lookup(peer, at(60s + 500ms)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.lookup(peer, at(61s + 500ms)), (Lookup{.mac = peer_mac, .send_request = true}));

    cache.learn(peer, peer_mac, at(62s));
    EXPECT_EQ(cache.lookup(peer, at(100s)), (Lookup{.mac = peer_mac, .send_request = false}))
        << "the refresh confirmed it";
}

TEST(ArpCache, AnExpiredEntryIsUnresolvedAgainAndStillKnown) {
    aloe::net::ArpCache cache{small};
    cache.learn(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_TRUE(cache.contains(peer)) << "a reply for it is not unsolicited";
    EXPECT_EQ(cache.lookup(peer, at(120s + 200ms)), (Lookup{.mac = std::nullopt, .send_request = false}));
    cache.learn(peer, other_mac, at(121s));
    EXPECT_EQ(cache.lookup(peer, at(122s)), (Lookup{.mac = other_mac, .send_request = false}))
        << "learn replaces the MAC";
}

TEST(ArpCache, AFullWindowReusesAnExpiredSlotBeforeEvictingTheOldestConfirmation) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < 8; ++index) {
        cache.learn(host(index), peer_mac, at(0s));
    }
    ASSERT_EQ(cache.size(), 8);
    cache.learn(host(0), peer_mac, at(100s));  // the one fresh entry

    // At 125 s hosts 1 to 7 are expired, host 0 is not: a new address takes an expired slot.
    for (std::uint8_t index = 8; index < 15; ++index) {
        cache.learn(host(index), other_mac, at(125s));
        EXPECT_TRUE(cache.contains(host(0))) << "a fresh entry is never evicted while an expired one exists";
    }
    EXPECT_EQ(cache.size(), 8);
    EXPECT_EQ(cache.lookup(host(0), at(125s)), (Lookup{.mac = peer_mac, .send_request = false}))
        << "confirmed 25 s ago: fresh, and still there";

    // No free slot, no expired slot: the oldest confirmation goes, and that is host 0 at 100 s.
    cache.learn(host(15), other_mac, at(126s));
    EXPECT_EQ(cache.size(), 8);
    EXPECT_FALSE(cache.contains(host(0)));
    EXPECT_TRUE(cache.contains(host(15)));
    for (std::uint8_t index = 8; index < 15; ++index) {
        EXPECT_TRUE(cache.contains(host(index)));
    }
}

TEST(ArpCache, AnIncompleteEntryIsTheFirstToGo) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.lookup(peer, at(0s));  // incomplete: asked, never answered
    for (std::uint8_t index = 0; index < 7; ++index) {
        cache.learn(host(index), peer_mac, at(1s));
    }
    ASSERT_EQ(cache.size(), 8);
    cache.learn(host(7), peer_mac, at(2s));
    EXPECT_FALSE(cache.contains(peer)) << "an entry with no confirmation is older than any confirmed one";
    EXPECT_EQ(cache.size(), 8);
}

TEST(ArpCache, ABadConfigThrows) {
    using aloe::net::ArpCache;
    using aloe::net::ArpCacheConfig;
    EXPECT_THROW((ArpCache{ArpCacheConfig{.capacity = 100}}), std::invalid_argument) << "not a power of two";
    EXPECT_THROW((ArpCache{ArpCacheConfig{.capacity = 4}}), std::invalid_argument) << "under the probe window";
    EXPECT_THROW((ArpCache{ArpCacheConfig{.reachable = 0s}}), std::invalid_argument);
    EXPECT_THROW((ArpCache{ArpCacheConfig{.request_interval = 0s}}), std::invalid_argument);
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.reachable = 120s, .expire = 120s}
    }),
                 std::invalid_argument)
        << "reachable must be below expire";
    EXPECT_NO_THROW((ArpCache{ArpCacheConfig{}}));
}
