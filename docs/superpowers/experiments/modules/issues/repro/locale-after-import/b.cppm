module;
#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>
#include <x86intrin.h>
export module probe:b;
import :a;
export namespace probe {
    struct B {
        int y;
    };
}  // namespace probe
