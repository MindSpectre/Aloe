#include <cstdint>
#include <string_view>

#include <eal_arguments.hpp>
#include <gtest/gtest.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>

// One process can start the EAL once, so everything is checked in one test.
TEST(DpdkEal, StartsUnprivilegedAndSeesTheNullDevice) {
    auto arguments = aloe::testing::unprivileged_eal_arguments();

    const int consumed = rte_eal_init(arguments.argc(), arguments.argv());
    ASSERT_GE(consumed, 0) << "rte_eal_init failed: " << rte_strerror(rte_errno);

    // A port exists only if the null driver registered itself, which for a
    // static build means its object files survived linking.
    ASSERT_EQ(rte_eth_dev_count_avail(), 1);

    std::uint16_t port = 0;
    ASSERT_EQ(rte_eth_dev_get_port_by_name("net_null0", &port), 0);

    rte_eth_dev_info info{};
    ASSERT_EQ(rte_eth_dev_info_get(port, &info), 0);
    EXPECT_EQ(std::string_view{info.driver_name}, "net_null");

    EXPECT_EQ(rte_eal_cleanup(), 0);
}
