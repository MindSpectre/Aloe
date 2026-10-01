#include <aloe/ethdev>
#include <string>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>

namespace {

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

}  // namespace

TEST(EthdevEal, TheEalStartsOncePerProcess) {
    const std::vector<std::string> options = aloe::testing::unprivileged_eal_options("net_null1");
    EXPECT_THROW(aloe::ethdev::Eal{options}, aloe::ethdev::EthdevError);
}
