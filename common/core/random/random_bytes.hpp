#pragma once

#include <cstddef>
#include <span>

namespace aloe::core {

    /// Fills `out` from the kernel's random source, `getrandom(2)`. Setup only: it may block until the kernel's pool
    /// is ready. Retries on `EINTR` and on a short read; throws `std::system_error` on any other failure.
    void random_bytes(std::span<std::byte> out);

}  // namespace aloe::core
