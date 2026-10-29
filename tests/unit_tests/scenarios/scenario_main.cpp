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

// Every .pkt under ALOE_SCENARIOS_DIR becomes one test per congestion-control policy its `--congestion` line allows
// and per checksum mode of the fabric, named after its path:
//
//     ./Aloe.Tests.Unit.Scenarios --gtest_filter='Scenario.tcp/close/*'
//
namespace {

    template <typename HarnessT, typename RunnerT>
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
            HarnessT harness{aloe::scenario::configured(aloe::scenario::HarnessConfig{.offloads = offloads_}, *script)};
            RunnerT runner{harness, *script};
            const auto result = runner.run();
            EXPECT_TRUE(result.has_value()) << aloe::scenario::format(result.error(), path_.string());
        }

    private:
        std::filesystem::path path_;
        aloe::fabric::EmulatedOffloads offloads_;
    };

    using NewRenoTest      = ScriptTest<aloe::scenario::Harness, aloe::scenario::Runner>;
    using NoCongestionTest = ScriptTest<aloe::scenario::NoCongestionHarness, aloe::scenario::NoCongestionRunner>;

    /// Registers `name` in both checksum modes of the fabric under the policy's name.
    template <typename TestT>
    void register_policy(const std::string& name, const std::string_view policy, const std::filesystem::path& path) {
        for (const auto& [offloads, mode] : {
                 std::pair{aloe::fabric::EmulatedOffloads::None,      "NoOffloads"},
                 std::pair{aloe::fabric::EmulatedOffloads::Checksums, "Checksums" }
        }) {
            ::testing::RegisterTest(
                "Scenario",
                (name + "/" + std::string{policy} + "/" + mode).c_str(),
                nullptr,
                nullptr,
                path.c_str(),
                1,
                [path, offloads = offloads]() -> ::testing::Test* { return new TestT{path, offloads}; });
        }
    }

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
            const auto text        = [&path] {
                std::ifstream file{path};
                return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            }();
            // An unparsable script registers under both policies; its tests report the parse error.
            const auto script = aloe::scenario::parse_script(text);
            const auto choice = script ? script->congestion : aloe::scenario::CongestionChoice::Both;
            if (choice != aloe::scenario::CongestionChoice::None) {
                register_policy<NewRenoTest>(name, "NewReno", path);
            }
            if (choice != aloe::scenario::CongestionChoice::NewReno) {
                register_policy<NoCongestionTest>(name, "NoCongestion", path);
            }
        }
    }

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    register_scripts(ALOE_SCENARIOS_DIR);
    return RUN_ALL_TESTS();
}
