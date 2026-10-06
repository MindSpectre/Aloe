#include <aloe/core>
#include <aloe/execution>
#include <cstdlib>
#include <print>
#include <tuple>

#include <rte_version.h>

int main() {
    const auto result = aloe::execution::ex::sync_wait(aloe::execution::ex::just(41) |
                                                       aloe::execution::ex::then([](int value) { return value + 1; }));
    if (!result.has_value()) {
        return EXIT_FAILURE;
    }

    std::println("Aloe {}", aloe::core::version_string);
    std::println("{}", rte_version());
    std::println("sender result {}", std::get<0>(*result));
    return EXIT_SUCCESS;
}
