#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace aloe::testing {

    /**
     * @brief Owns an argv for rte_eal_init.
     *
     * rte_eal_init takes `char**` and may reorder the pointers, so the
     * arguments must be mutable and must outlive the call.
     */
    class EalArguments {
    public:
        EalArguments(std::initializer_list<std::string_view> arguments) {
            storage_.reserve(arguments.size());
            for (const std::string_view argument : arguments) {
                storage_.emplace_back(argument);
            }
            pointers_.reserve(storage_.size());
            for (std::string& argument : storage_) {
                pointers_.push_back(argument.data());
            }
        }

        [[nodiscard]] int argc() const noexcept {
            return static_cast<int>(pointers_.size());
        }

        [[nodiscard]] char** argv() noexcept {
            return pointers_.data();
        }

    private:
        std::vector<std::string> storage_;
        std::vector<char*> pointers_;
    };

    /**
     * @brief Arguments that start the EAL with no privileges.
     *
     * No hugepages, no PCI scan, no files shared with other DPDK processes, and
     * one null virtual device, so the process needs neither root nor a NIC.
     */
    [[nodiscard]] inline EalArguments unprivileged_eal_arguments() {
        return EalArguments{"aloe-test",
                            "--no-huge",
                            "--no-pci",
                            "--in-memory",
                            "--no-telemetry",
                            "-m",
                            "64",
                            "-l",
                            "0",
                            "--vdev=net_null0"};
    }

    /**
     * @brief The same unprivileged options as a list of strings, with `vdev` as the one virtual
     * device, for aloe::ethdev::Eal.
     */
    [[nodiscard]] inline std::vector<std::string> unprivileged_eal_options(std::string_view vdev) {
        return {"aloe-test",
                "--no-huge",
                "--no-pci",
                "--in-memory",
                "--no-telemetry",
                "-m",
                "64",
                "-l",
                "0",
                "--vdev=" + std::string{vdev}};
    }

}  // namespace aloe::testing
