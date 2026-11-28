#include <aloe/net>
#include <aloe/wire>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using TimePoint = aloe::core::TimePoint;
    using Lookup    = aloe::net::ArpCache::Lookup;
    using Taught    = aloe::net::ArpCache::Taught;

    constexpr TimePoint start{};

    [[nodiscard]] TimePoint at(const std::chrono::nanoseconds offset) {
        return start + offset;
    }

    constexpr aloe::wire::Ipv4Address peer{10, 0, 0, 1};
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr aloe::wire::MacAddress other_mac{0x02, 0, 0, 0, 0, 0x02};

    /// Eight entries, learning on: the ninth address finds the table full.
    constexpr aloe::net::ArpCacheConfig small{
        .capacity = 8, .reachable = 60s, .expire = 120s, .request_interval = 1s, .learning = true};
    constexpr std::size_t held = 8;
    constexpr Lookup refused{.mac = std::nullopt, .send_request = false, .permanent = false, .full = true};

    [[nodiscard]] aloe::wire::Ipv4Address host(const std::uint8_t index) {
        return {10, 0, 1, index};
    }

    /// `count` addresses of 10.1.0.0/16, apart from `peer` and every `host`, that share their home slot in any table of
    /// up to 256 slots, `small`'s included: the high bits of the hash pick the home, so a home shared in 256 slots is
    /// shared in fewer.
    [[nodiscard]] std::vector<aloe::wire::Ipv4Address> sharing_one_home(const std::size_t count) {
        constexpr std::size_t slots = 256;
        std::vector<aloe::wire::Ipv4Address> found;
        std::size_t home = slots;
        for (std::uint32_t low = 1; found.size() < count && low < 0x10000U; ++low) {
            const auto address         = aloe::wire::Ipv4Address::from_uint32(0x0a010000U | low);
            const std::size_t its_home = aloe::net::detail::arp_home_slot(address, slots);
            if (home == slots) {
                home = its_home;
            }
            if (its_home == home) {
                found.push_back(address);
            }
        }
        return found;
    }

    /// The slots of `small`'s table: the next power of two at or above twice its capacity.
    constexpr std::size_t small_slots = std::bit_ceil(2 * small.capacity);

    /// An address of 10.1.0.0/16, apart from `peer`, every `host` and `skip` others, whose home in `small`'s table is
    /// `slot`, taken modulo the slots.
    [[nodiscard]] aloe::wire::Ipv4Address homed_at(const std::size_t slot, std::size_t skip = 0) {
        for (std::uint32_t low = 1; low < 0x10000U; ++low) {
            const auto address = aloe::wire::Ipv4Address::from_uint32(0x0a010000U | low);
            if (aloe::net::detail::arp_home_slot(address, small_slots) != slot % small_slots) {
                continue;
            }
            if (skip == 0) {
                return address;
            }
            --skip;
        }
        ADD_FAILURE() << "no address of 10.1.0.0/16 is homed at slot " << slot;
        return {};
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

TEST(ArpCache, AnUpdateResolvesAndAReachableEntryAsksForNothing) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.lookup(peer, at(0s));
    EXPECT_EQ(cache.update(peer, peer_mac, at(1ms)), Taught::Updated);
    EXPECT_EQ(cache.lookup(peer, at(30s)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.lookup(peer, at(59s)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.size(), 1);
}

TEST(ArpCache, ASeedWithoutALookupInsertsAReachableEntry) {
    aloe::net::ArpCache cache{small};
    EXPECT_EQ(cache.seed(peer, peer_mac, at(0s)), Taught::Created);
    EXPECT_EQ(cache.lookup(peer, at(1s)), (Lookup{.mac = peer_mac, .send_request = false}));
}

TEST(ArpCache, AStaleEntryKeepsItsMacAndRefreshesOncePerInterval) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.seed(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.lookup(peer, at(60s)), (Lookup{.mac = peer_mac, .send_request = true}))
        << "stale: still usable, and a refresh is due";
    EXPECT_EQ(cache.lookup(peer, at(60s + 500ms)), (Lookup{.mac = peer_mac, .send_request = false}));
    EXPECT_EQ(cache.lookup(peer, at(61s + 500ms)), (Lookup{.mac = peer_mac, .send_request = true}));

    EXPECT_EQ(cache.update(peer, peer_mac, at(62s)), Taught::Updated);
    EXPECT_EQ(cache.lookup(peer, at(100s)), (Lookup{.mac = peer_mac, .send_request = false}))
        << "the refresh confirmed it";
}

TEST(ArpCache, AnExpiredEntryIsUnresolvedAgainAndStillKnown) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.seed(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_TRUE(cache.contains(peer)) << "a reply for it is not unsolicited";
    EXPECT_EQ(cache.lookup(peer, at(120s + 200ms)), (Lookup{.mac = std::nullopt, .send_request = false}));
    EXPECT_EQ(cache.update(peer, other_mac, at(121s)), Taught::Updated);
    EXPECT_EQ(cache.lookup(peer, at(122s)), (Lookup{.mac = other_mac, .send_request = false}))
        << "the update replaces the MAC";
}

TEST(ArpCache, AnUpdateForAnAddressNotHeldStoresNothing) {
    aloe::net::ArpCache cache{small};
    EXPECT_EQ(cache.update(peer, peer_mac, at(0s)), Taught::Unknown);
    EXPECT_FALSE(cache.contains(peer));
    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.generation(), 0U);
}

TEST(ArpCache, AnUpdateFillsAnEntryALookupMade) {
    aloe::net::ArpCache cache{small};
    EXPECT_EQ(cache.lookup(peer, at(0s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_EQ(cache.update(peer, peer_mac, at(1ms)), Taught::Updated);
    EXPECT_EQ(cache.lookup(peer, at(2ms)), (Lookup{.mac = peer_mac, .send_request = false}));
}

TEST(ArpCache, ASeedCreatesAndALaterUpdateChangesIt) {
    aloe::net::ArpCache cache{small};
    const std::uint64_t before = cache.generation();
    EXPECT_EQ(cache.seed(peer, peer_mac, at(0s)), Taught::Created);
    EXPECT_EQ(cache.update(peer, other_mac, at(1s)), Taught::Updated);
    EXPECT_EQ(cache.generation(), before + 2) << "a MAC appeared, then another took its place";
    EXPECT_EQ(cache.lookup(peer, at(2s)), (Lookup{.mac = other_mac, .send_request = false}));
    EXPECT_EQ(cache.size(), 1);
}

TEST(ArpCache, AnyFreeSlotServesAnyAddress) {
    aloe::net::ArpCache cache{small};
    const std::vector<aloe::wire::Ipv4Address> chain = sharing_one_home(held);
    ASSERT_EQ(chain.size(), held);
    for (const aloe::wire::Ipv4Address address : chain) {
        EXPECT_EQ(cache.lookup(address, at(0s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    }
    EXPECT_EQ(cache.size(), held);
    for (const aloe::wire::Ipv4Address address : chain) {
        EXPECT_TRUE(cache.contains(address)) << "no window bounds how far an address probes from its home";
    }
}

TEST(ArpCache, AFullTableOfResolvedEntriesRefusesAndKeepsEveryOne) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        ASSERT_EQ(cache.seed(host(index), peer_mac, at(0s)), Taught::Created);
        ASSERT_EQ(cache.lookup(host(index), at(0s)), (Lookup{.mac = peer_mac, .send_request = false}));
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        EXPECT_EQ(cache.lookup(peer, at(1s)), refused) << "no slot, and no request asked for";
    }
    EXPECT_EQ(cache.seed(peer, other_mac, at(1s)), Taught::Full);
    EXPECT_FALSE(cache.contains(peer));
    EXPECT_EQ(cache.size(), held);
    for (std::uint8_t index = 0; index < held; ++index) {
        EXPECT_EQ(cache.lookup(host(index), at(2s)), (Lookup{.mac = peer_mac, .send_request = false}));
    }
}

TEST(ArpCache, AFullTableOfWaitingEntriesRefusesUntilOneLapses) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        ASSERT_EQ(cache.lookup(host(index), at(0s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    }
    EXPECT_EQ(cache.lookup(peer, at(0s)), refused) << "every request is still waiting for its reply";
    EXPECT_EQ(cache.lookup(peer, at(1s) - 1ns), refused);
    EXPECT_EQ(cache.lookup(peer, at(1s)), (Lookup{.mac = std::nullopt, .send_request = true}))
        << "a request interval on, a slot that never resolved is given up";
    EXPECT_TRUE(cache.contains(peer));
    EXPECT_EQ(cache.size(), held);
}

TEST(ArpCache, AnExpiredEntrysSlotIsUsedAgain) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        ASSERT_EQ(cache.seed(host(index), peer_mac, at(0s)), Taught::Created);
    }
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_TRUE(cache.contains(peer));
    EXPECT_EQ(cache.size(), held);
}

TEST(ArpCache, AResolvedEntryIsNeverRemovedBeforeItExpires) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        ASSERT_EQ(cache.seed(host(index), peer_mac, at(std::chrono::seconds{index})), Taught::Created);
    }
    // Host 0, confirmed at 0 s, is the first to expire, at 120 s; stale ones from 60 s on are still resolved.
    for (auto stamp = std::chrono::seconds{held}; stamp < 120s; ++stamp) {
        EXPECT_EQ(cache.lookup(peer, at(stamp)), refused) << "at " << stamp.count() << " s";
    }
    EXPECT_EQ(cache.lookup(peer, at(120s - 1ns)), refused);
    for (std::uint8_t index = 0; index < held; ++index) {
        EXPECT_TRUE(cache.contains(host(index)));
    }
    EXPECT_EQ(cache.size(), held);
}

TEST(ArpCache, AFullTableGivesUpAWaitingSlotBeforeAnExpiredOne) {
    aloe::net::ArpCache cache{small};
    // Seven resolved at homes 0 to 6, ahead of the waiting one at home 15 in slot order.
    for (std::size_t slot = 0; slot < held - 1; ++slot) {
        ASSERT_EQ(cache.seed(homed_at(slot), peer_mac, at(0s)), Taught::Created);
    }
    const aloe::wire::Ipv4Address waiting = homed_at(small_slots - 1);
    ASSERT_EQ(cache.lookup(waiting, at(119s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    const std::uint64_t before = cache.generation();

    // At 120 s the seven have expired and the request has lapsed: the slot that holds no MAC goes.
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_FALSE(cache.contains(waiting));
    for (std::size_t slot = 0; slot < held - 1; ++slot) {
        EXPECT_TRUE(cache.contains(homed_at(slot)));
    }
    EXPECT_EQ(cache.generation(), before) << "no MAC went";
}

// The design's rule 1, as amended: "It extends the entry's life only when it answers a request the brick itself has
// outstanding for that address: a frame nobody asked for never moves the confirmation stamp".
TEST(ArpCache, AFrameNobodyAskedForDoesNotExtendAnEntrysLife) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.lookup(peer, at(0s));
    ASSERT_EQ(cache.update(peer, peer_mac, at(0s)), Taught::Updated);  // the answer to the brick's request
    EXPECT_EQ(cache.update(peer, peer_mac, at(100s)), Taught::Updated) << "still reported";
    EXPECT_EQ(cache.confirmed(peer), at(0s));
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}))
        << "expired, as if no frame had named it";
}

TEST(ArpCache, AnAnswerToOurRefreshExtendsIt) {
    aloe::net::ArpCache cache{small};
    std::ignore = cache.seed(peer, peer_mac, at(0s));
    ASSERT_EQ(cache.lookup(peer, at(61s)), (Lookup{.mac = peer_mac, .send_request = true})) << "the refresh";
    EXPECT_EQ(cache.update(peer, peer_mac, at(61s + 500ms)), Taught::Updated);
    EXPECT_EQ(cache.confirmed(peer), at(61s + 500ms));
    EXPECT_EQ(cache.lookup(peer, at(100s)), (Lookup{.mac = peer_mac, .send_request = false})) << "fresh again";
    EXPECT_EQ(cache.lookup(peer, at(150s)).mac, peer_mac) << "past the first confirmation's expiry";
}

TEST(ArpCache, AFrameNobodyAskedForCanChangeTheMacAndNotTheStamp) {
    aloe::net::ArpCache cache{small};
    std::ignore                = cache.seed(peer, peer_mac, at(0s));
    const std::uint64_t before = cache.generation();
    EXPECT_EQ(cache.update(peer, other_mac, at(30s)), Taught::Updated);
    EXPECT_EQ(cache.generation(), before + 1) << "the MAC changed";
    EXPECT_EQ(cache.confirmed(peer), at(0s));
    EXPECT_EQ(cache.lookup(peer, at(31s)), (Lookup{.mac = other_mac, .send_request = false}));
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
}

TEST(ArpCache, ATableNamedByFramesStillFreesItsSlots) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        ASSERT_EQ(cache.seed(host(index), peer_mac, at(0s)), Taught::Created);
    }
    // Whoever keeps naming every held address, every 10 s, without the brick asking.
    for (auto stamp = 10s; stamp <= 120s; stamp += 10s) {
        for (std::uint8_t index = 0; index < held; ++index) {
            ASSERT_EQ(cache.update(host(index), peer_mac, at(stamp)), Taught::Updated);
        }
    }
    EXPECT_EQ(cache.lookup(peer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}))
        << "the entries expired all the same, so a new neighbour finds a slot";
    EXPECT_TRUE(cache.contains(peer));
    EXPECT_EQ(cache.size(), held);
}

TEST(ArpCache, RemovingAnEntryKeepsTheChainsOfTheOthers) {
    // In `small`'s 16 slots, from home slot 13, so the chain wraps past the last slot. Seeded in this order:
    //   13: `first`,   home 13
    //   14: `lapsing`, home 13, confirmed earliest, so it alone has expired at 120 s
    //   15: `settled`, home 15, at its home: it must stay, since its probe starts there
    //    0: `behind`,  home 13, three past its home: it must move back into the hole
    //    1: `next`,    home 14, pushed past `settled` and `behind`: it must move back to slot 0
    // and three more at their own homes, 5, 6 and 7. Slot 2 ends the chain; the newcomer's home, 9, is free, so its
    // insert fills nothing the removal left. A removal that only freed slot 14 would cut the chain from 13 there, and
    // `behind` and `next` would be lost; one that moved every entry back would put `settled` before its home, where its
    // probe never looks.
    constexpr std::size_t home            = 13;
    const aloe::wire::Ipv4Address first   = homed_at(home);
    const aloe::wire::Ipv4Address lapsing = homed_at(home, 1);
    const aloe::wire::Ipv4Address settled = homed_at(home + 2);
    const aloe::wire::Ipv4Address behind  = homed_at(home, 2);
    const aloe::wire::Ipv4Address next    = homed_at(home + 1);
    const std::vector<aloe::wire::Ipv4Address> kept{
        first, settled, behind, next, homed_at(home + 8), homed_at(home + 9), homed_at(home + 10)};
    const aloe::wire::Ipv4Address newcomer = homed_at(home + 12);

    aloe::net::ArpCache cache{small};
    ASSERT_EQ(cache.seed(first, peer_mac, at(10s)), Taught::Created);
    ASSERT_EQ(cache.seed(lapsing, other_mac, at(0s)), Taught::Created);
    for (std::size_t index = 1; index < kept.size(); ++index) {
        ASSERT_EQ(cache.seed(kept[index], peer_mac, at(10s)), Taught::Created);
    }
    ASSERT_EQ(cache.size(), held);

    EXPECT_EQ(cache.lookup(newcomer, at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_FALSE(cache.contains(lapsing));
    EXPECT_TRUE(cache.contains(newcomer));
    EXPECT_EQ(cache.size(), held);
    EXPECT_EQ(cache.confirmed(first), at(10s)) << "the chain's first entry";
    EXPECT_EQ(cache.confirmed(behind), at(10s)) << "the entry behind the hole whose home is before it";
    EXPECT_EQ(cache.confirmed(settled), at(10s)) << "the entry behind the hole that sits at its own home";
    EXPECT_EQ(cache.confirmed(next), at(10s)) << "the neighbour whose home follows the chain's";
    for (std::size_t index = 4; index < kept.size(); ++index) {
        EXPECT_EQ(cache.confirmed(kept[index]), at(10s)) << "an entry away from the chain";
    }
}

TEST(ArpCache, WithLearningOffTheTableIsNeverUsed) {
    aloe::net::ArpCacheConfig off = small;
    off.learning                  = false;
    aloe::net::ArpCache cache{off};
    EXPECT_EQ(cache.seed(peer, peer_mac, at(0s)), Taught::Off);
    EXPECT_EQ(cache.update(peer, peer_mac, at(0s)), Taught::Off);
    EXPECT_EQ(cache.lookup(peer, at(0s)), Lookup{}) << "nothing, and no request";
    EXPECT_FALSE(cache.contains(peer));
    EXPECT_EQ(cache.size(), 0);
    EXPECT_EQ(cache.capacity(), 0);
    EXPECT_EQ(cache.generation(), 0U);
}

TEST(ArpCache, AStaticAddressIsNeitherSeededNorUpdated) {
    aloe::net::ArpCache cache{small, {{.address = peer, .mac = peer_mac}}};
    EXPECT_EQ(cache.seed(peer, peer_mac, at(0s)), Taught::Static);
    EXPECT_EQ(cache.update(peer, other_mac, at(0s)), Taught::Conflict);
    EXPECT_EQ(cache.seed(peer, other_mac, at(0s)), Taught::Conflict);
    EXPECT_EQ(cache.lookup(peer, at(0s)), (Lookup{.mac = peer_mac, .send_request = false, .permanent = true}));
    EXPECT_FALSE(cache.contains(peer)) << "a static entry is not a learned one";
    EXPECT_EQ(cache.generation(), 0U);
}

TEST(ArpCache, ABadConfigThrows) {
    using aloe::net::ArpCache;
    using aloe::net::ArpCacheConfig;
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.capacity = 0, .learning = true}
    }),
                 std::invalid_argument)
        << "no entry at all";
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.capacity = (std::size_t{1} << 30U) + 1, .learning = true}
    }),
                 std::invalid_argument)
        << "twice the entries must fit the table's 32-bit hash";
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.reachable = 0s, .learning = true}
    }),
                 std::invalid_argument);
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.request_interval = 0s, .learning = true}
    }),
                 std::invalid_argument);
    EXPECT_THROW((ArpCache{
                     ArpCacheConfig{.reachable = 120s, .expire = 120s, .learning = true}
    }),
                 std::invalid_argument)
        << "reachable must be below expire";
    // A stamp plus a lifetime must not overflow: each lifetime fits a quarter of core::Duration, with its own refusal.
    const auto refusal = [](const ArpCacheConfig& config) {
        try {
            const ArpCache cache{config};
        } catch (const std::invalid_argument& error) {
            return std::string{error.what()};
        }
        return std::string{};
    };
    constexpr aloe::core::Duration quarter = aloe::core::Duration::max() / 4;
    const std::string reachable =
        refusal({.reachable = quarter + 1ns, .expire = aloe::core::Duration::max() / 2, .learning = true});
    const std::string expire = refusal({.expire = quarter + 1ns, .learning = true});
    EXPECT_NE(reachable.find("quarter"), std::string::npos) << reachable;
    EXPECT_NE(expire.find("quarter"), std::string::npos) << expire;
    EXPECT_NE(reachable, expire);
    EXPECT_NO_THROW((ArpCache{
        ArpCacheConfig{.reachable = std::chrono::hours{24}, .expire = std::chrono::hours{48}, .learning = true}
    }))
        << "the benchmark's lifetimes";
    EXPECT_NO_THROW((ArpCache{
        ArpCacheConfig{.capacity = 100, .learning = true}
    }))
        << "any positive capacity";
    EXPECT_NO_THROW((ArpCache{
        ArpCacheConfig{.capacity = 0, .learning = false}
    }))
        << "with learning off the learned table's settings are not checked";
    EXPECT_NO_THROW((ArpCache{ArpCacheConfig{.learning = true}}));
}

TEST(ArpCache, ConfirmedReportsTheStampOfAReachableOrStaleEntry) {
    aloe::net::ArpCache cache{small};
    EXPECT_EQ(cache.confirmed(peer), std::nullopt);
    std::ignore = cache.lookup(peer, at(0s));  // incomplete
    EXPECT_EQ(cache.confirmed(peer), std::nullopt);
    EXPECT_EQ(cache.update(peer, peer_mac, at(5s)), Taught::Updated);
    EXPECT_EQ(cache.confirmed(peer), at(5s));
    std::ignore = cache.lookup(peer, at(70s));  // stale, still confirmed at 5 s
    EXPECT_EQ(cache.confirmed(peer), at(5s));
}

TEST(ArpCache, TheGenerationMovesWhenAMacChangesOrGoes) {
    aloe::net::ArpCache cache{small};
    EXPECT_EQ(cache.generation(), 0U);
    std::ignore = cache.seed(peer, peer_mac, at(0s));
    EXPECT_EQ(cache.generation(), 1U) << "a MAC appeared";
    std::ignore = cache.update(peer, peer_mac, at(30s));
    EXPECT_EQ(cache.generation(), 1U) << "a frame naming the same MAC changes nothing";
    std::ignore = cache.update(peer, other_mac, at(31s));
    EXPECT_EQ(cache.generation(), 2U) << "a new MAC";
    EXPECT_EQ(cache.lookup(peer, at(200s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_EQ(cache.generation(), 3U) << "expiry observed: the MAC went";
    EXPECT_EQ(cache.confirmed(peer), std::nullopt) << "the entry is incomplete now";
    EXPECT_EQ(cache.lookup(peer, at(200s + 500ms)), (Lookup{.mac = std::nullopt, .send_request = false}));
    EXPECT_EQ(cache.generation(), 3U) << "a second look at an incomplete entry moves nothing";
}

// Derived again: an entry no longer evicts a reachable one (the design's rule 2, "a resolved entry leaves it only by
// expiring"); the slot of an expired one is taken instead, and its MAC goes with it.
TEST(ArpCache, TakingAnExpiredEntrysSlotMovesTheGeneration) {
    aloe::net::ArpCache cache{small};
    for (std::uint8_t index = 0; index < held; ++index) {
        std::ignore = cache.seed(host(index), peer_mac, at(0s));
    }
    ASSERT_EQ(cache.size(), held);
    const std::uint64_t before = cache.generation();
    // No free slot, every entry expired and none looked at since: the lookup of a ninth address takes one's slot.
    EXPECT_EQ(cache.lookup(host(8), at(120s)), (Lookup{.mac = std::nullopt, .send_request = true}));
    EXPECT_TRUE(cache.contains(host(8)));
    EXPECT_EQ(cache.size(), held);
    EXPECT_EQ(cache.generation(), before + 1)
        << "the expired entry's MAC went: an Ipv4Prebuilt of its generation is stale";
    const std::uint64_t taken = cache.generation();
    // Host 8's request is still waiting, so the seed takes another expired entry's slot, and brings a MAC of its own.
    EXPECT_EQ(cache.seed(host(9), other_mac, at(120s + 500ms)), Taught::Created);
    EXPECT_TRUE(cache.contains(host(8)));
    EXPECT_EQ(cache.generation(), taken + 2);
}
