#include <aloe/core>
#include <type_traits>

#include <gtest/gtest.h>

TEST(ValueOr, ReturnsTheValueWhenItConvertsToTrue) {
    EXPECT_EQ(aloe::core::value_or(5, 7), 5);
    EXPECT_EQ(aloe::core::value_or(0, 7), 7);
    static_assert(aloe::core::value_or(0, 7) == 7);
    const char* const nothing = nullptr;
    EXPECT_STREQ(aloe::core::value_or(nothing, "fallback"), "fallback");
}

TEST(ValueOr, YieldsTheLvalueItselfWhenBothOperandsAreLvaluesOfOneType) {
    int first  = 1;
    int second = 2;
    static_assert(std::is_same_v<decltype(aloe::core::value_or(first, second)), int&>);
    EXPECT_EQ(&aloe::core::value_or(first, second), &first) << "no copy, like `first ?: second`";
    first = 0;
    EXPECT_EQ(&aloe::core::value_or(first, second), &second);
}

TEST(ValueOr, YieldsAValueWhenAnOperandIsATemporary) {
    int first = 0;
    static_assert(std::is_same_v<decltype(aloe::core::value_or(first, 9)), int>);
    EXPECT_EQ(aloe::core::value_or(first, 9), 9);
}
