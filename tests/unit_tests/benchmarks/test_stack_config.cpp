#include <chrono>

#include <gtest/gtest.h>
#include <level_bricks.hpp>

TEST(StackConfig, ArpNeverGoesStaleDuringARun) {
    const aloe::benchmarks::PeerConfig peer{};
    const aloe::net::Ipv4Config config = aloe::benchmarks::stack_ip_config(peer);
    EXPECT_EQ(config.address, peer.stack_ip);
    EXPECT_EQ(config.prefix, 24U);
    EXPECT_GE(config.arp_reachable, std::chrono::hours{24})
        << "a gapped run can last minutes; no refresh on the hot path";
    EXPECT_GT(config.arp_expire, config.arp_reachable);
}
