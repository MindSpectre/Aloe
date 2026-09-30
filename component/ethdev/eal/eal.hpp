#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aloe::ethdev {

    /// Thrown by setup and teardown of the DPDK backend, never on the hot path.
    class EthdevError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /**
     * @brief The DPDK environment abstraction layer, started once per process.
     *
     * The constructor copies the arguments, because `rte_eal_init` needs mutable storage that
     * outlives the call, and starts the EAL. The destructor cleans it up. DPDK cannot start again
     * after that, so a second construction throws whether or not the first is still alive. The
     * runtime or the application constructs it once; tests construct it in a gtest environment.
     */
    class Eal {
    public:
        explicit Eal(std::span<const std::string> arguments);
        Eal(const Eal&)            = delete;
        Eal& operator=(const Eal&) = delete;
        Eal(Eal&&)                 = delete;
        Eal& operator=(Eal&&)      = delete;
        ~Eal();
    };

    namespace detail {

        /// `what` followed by DPDK's text for `error`, an `rte_errno` value or a negated return code.
        [[nodiscard]] std::string describe(std::string_view what, int error);

    }  // namespace detail

}  // namespace aloe::ethdev
