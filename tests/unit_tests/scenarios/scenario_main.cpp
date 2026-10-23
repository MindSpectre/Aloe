#include <algorithm>
#include <aloe/fabric>
#include <aloe/scenario>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Every .pkt under ALOE_SCENARIOS_DIR becomes one test per checksum mode of the fabric, named after its path:
//
//     ./Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'
//
namespace {

    class ScriptTest : public ::testing::Test {
    public:
        ScriptTest(std::filesystem::path path, const aloe::fabric::EmulatedOffloads offloads)
            : path_{std::move(path)},
              offloads_{offloads} {
        }

        void TestBody() override {
            std::ifstream file{path_};
            ASSERT_TRUE(file.is_open()) << "cannot open " << path_;
            const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            const auto script = aloe::scenario::parse_script(text);
            ASSERT_TRUE(script.has_value()) << path_.string() << ":" << script.error().line << ":"
                                            << script.error().column << ": " << script.error().message;
            aloe::scenario::HarnessConfig config;
            config.offloads = offloads_;
            aloe::scenario::Harness harness{config};
            aloe::scenario::Runner runner{harness, *script};
            const auto result = runner.run();
            EXPECT_TRUE(result.has_value()) << aloe::scenario::format(result.error(), path_.string());
        }

    private:
        std::filesystem::path path_;
        aloe::fabric::EmulatedOffloads offloads_;
    };

    void register_scripts(const std::filesystem::path& root) {
        std::vector<std::filesystem::path> scripts;
        for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
            if (entry.is_regular_file() && entry.path().extension() == ".pkt") {
                scripts.push_back(entry.path());
            }
        }
        std::ranges::sort(scripts);
        for (const auto& path : scripts) {
            const std::string name = std::filesystem::relative(path, root).replace_extension().generic_string();
            for (const auto& [offloads, mode] : {
                     std::pair{aloe::fabric::EmulatedOffloads::None,      "NoOffloads"},
                     std::pair{aloe::fabric::EmulatedOffloads::Checksums, "Checksums" }
            }) {
                ::testing::RegisterTest(
                    "Scenario",
                    (name + "/" + mode).c_str(),
                    nullptr,
                    nullptr,
                    path.c_str(),
                    1,
                    [path, offloads = offloads]() -> ::testing::Test* { return new ScriptTest{path, offloads}; });
            }
        }
    }

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    register_scripts(ALOE_SCENARIOS_DIR);
    return RUN_ALL_TESTS();
}
