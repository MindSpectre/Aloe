// The umbrella header comes first on purpose: it must compile with nothing
// included before it.
#include <aloe/core>
#include <format>
#include <string_view>

#include <gtest/gtest.h>

namespace aloe::testing {
    // Defined in test_umbrella_second_unit.cpp, which includes the umbrella too.
    const std::string_view* version_object_seen_by_second_unit() noexcept;
}  // namespace aloe::testing

TEST(Version, MatchesTheProjectVersion) {
    EXPECT_EQ(aloe::version_string, ALOE_EXPECTED_VERSION);
}

TEST(Version, NumericPartsAgreeWithTheString) {
    EXPECT_EQ(std::format("{}.{}.{}", aloe::version_major, aloe::version_minor, aloe::version_patch),
              aloe::version_string);
}

// Two translation units include the umbrella and link into one program. That
// fails to link if a header defines something that is not inline, and the
// object must be the same one in both units.
TEST(Umbrella, TwoTranslationUnitsShareOneObject) {
    EXPECT_EQ(aloe::testing::version_object_seen_by_second_unit(), &aloe::version_string);
}
