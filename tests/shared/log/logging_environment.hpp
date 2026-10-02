#pragma once

#include <aloe/core>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace aloe::testing {

    /**
     * @brief Starts logging once for a test binary, at Warning so a passing suite stays quiet.
     *
     * Register it from a static initializer in one test file of the binary:
     *
     *     const auto* const logging = ::testing::AddGlobalTestEnvironment(new aloe::testing::LoggingEnvironment{});
     */
    class LoggingEnvironment : public ::testing::Environment {
    public:
        explicit LoggingEnvironment(core::LoggingConfig config = {.level = core::LogLevel::Warning})
            : config_{std::move(config)} {
        }

        void SetUp() override {
            logging_.emplace(config_);
        }

        void TearDown() override {
            logging_.reset();
        }

    private:
        core::LoggingConfig config_;
        std::optional<core::Logging> logging_;
    };

}  // namespace aloe::testing
