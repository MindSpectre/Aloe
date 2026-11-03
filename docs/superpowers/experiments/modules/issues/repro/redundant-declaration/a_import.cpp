#include <array>
#include <cstdint>
#include <span>
import probe.wire;

int main() {
    constexpr probe::wire::MacAddress mac{.bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    const std::array<std::uint8_t, 3> values{1, 2, 3};
    return mac.is_broadcast() && probe::wire::sum(std::span<const std::uint8_t>{values}) == 6 ? 0 : 1;
}
