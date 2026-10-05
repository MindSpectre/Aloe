#include <aloe/log>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

namespace {

    std::string read_all(const std::filesystem::path& path) {
        std::ifstream in{path};
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path unique_log_path() {
        return std::filesystem::temp_directory_path() / ("aloe-log-" + std::to_string(::getpid()) + ".txt");
    }

}  // namespace

// One Logging per process, so this binary holds exactly one test that constructs it.
TEST(Log, LinesReachTheSinkWithLevelLoggerNameAndArguments) {
    const auto path = unique_log_path();
    {
        aloe::log::Logging logging{
            {.level = aloe::log::LogLevel::Debug, .file = path}
        };
        auto log = aloe::log::logger("aloe.test");
        log.info<"shard {} starting on cpu {}">(3, 7U);
        log.trace<"filtered out at Debug">();
        log.flush();
    }
    const std::string text = read_all(path);
    std::filesystem::remove(path);

    EXPECT_NE(text.find("LOG_INFO"), std::string::npos) << text;
    EXPECT_NE(text.find("aloe.test"), std::string::npos) << text;
    EXPECT_NE(text.find("shard 3 starting on cpu 7"), std::string::npos) << text;
    EXPECT_EQ(text.find("filtered out"), std::string::npos) << text;
}
