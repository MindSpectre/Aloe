#include <aloe/core>
#include <chrono>
#include <type_traits>

#include <gtest/gtest.h>

// USE_TSC_CLOCK reaches the code through a generated header; CMake hands the test the same option directly.
TEST(Clock, IsTheOneTheBuildChose) {
    EXPECT_EQ((std::is_same_v<aloe::core::Clock, aloe::core::TscClock>), ALOE_EXPECTED_TSC_CLOCK != 0);
    EXPECT_EQ((std::is_same_v<aloe::core::Clock, std::chrono::steady_clock>), ALOE_EXPECTED_TSC_CLOCK == 0);
}
