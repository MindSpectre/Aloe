#pragma once

#include <aloe/ethdev>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <eal_arguments.hpp>
#include <gtest/gtest.h>
#include <rte_dev.h>

namespace aloe::testing {

    /**
     * @brief Starts the EAL once for a test binary, with one virtual device.
     *
     * Register it from a static initializer in the test file:
     *
     *     const auto* const environment =
     *         ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_ring0"});
     */
    class EalEnvironment : public ::testing::Environment {
    public:
        explicit EalEnvironment(std::string vdev)
            : vdev_{std::move(vdev)} {
        }

        void SetUp() override {
            const std::vector<std::string> options = unprivileged_eal_options(vdev_);
            eal_.emplace(options);
        }

        void TearDown() override {
            eal_.reset();
        }

    private:
        std::string vdev_;
        std::optional<ethdev::Eal> eal_;
    };

    /**
     * @brief Probes a fresh virtual device of `driver` and returns its name.
     *
     * A closed DPDK port cannot be reopened, so a test that constructs and destroys a Port takes
     * a device of its own: `probe_vdev("net_ring")` yields `net_ring100`, then `net_ring101`.
     */
    [[nodiscard]] inline std::string probe_vdev(std::string_view driver, std::string_view arguments = "") {
        static int next           = 100;
        const std::string name    = std::string{driver} + std::to_string(next++);
        const std::string devargs = arguments.empty() ? name : name + "," + std::string{arguments};
        if (const int result = rte_dev_probe(devargs.c_str()); result < 0) {
            throw std::runtime_error{ethdev::detail::describe("rte_dev_probe(" + devargs + ")", result)};
        }
        return name;
    }

}  // namespace aloe::testing
