#include <atomic>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <eal.hpp>
#include <rte_eal.h>
#include <rte_errno.h>

namespace aloe::ethdev {

    namespace {

        std::atomic<bool>& started_flag() noexcept {
            static std::atomic<bool> started{false};
            return started;
        }

    }  // namespace

    std::string detail::describe(std::string_view what, int error) {
        return std::format("{}: {}", what, rte_strerror(error < 0 ? -error : error));
    }

    Eal::Eal(std::span<const std::string> arguments) {
        if (started_flag().exchange(true)) {
            throw EthdevError{"the EAL can be started once per process"};
        }
        std::vector<std::string> storage(arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        if (rte_eal_init(static_cast<int>(storage.size()), argv.data()) < 0) {
            throw EthdevError{detail::describe("rte_eal_init", rte_errno)};
        }
    }

    Eal::~Eal() {
        rte_eal_cleanup();
    }

}  // namespace aloe::ethdev
