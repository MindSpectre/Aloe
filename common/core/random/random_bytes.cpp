#include "random_bytes.hpp"

#include <cerrno>
#include <cstddef>
#include <span>
#include <system_error>

#include <sys/random.h>

namespace aloe::core {

    void random_bytes(std::span<std::byte> out) {
        std::size_t filled = 0;
        while (filled < out.size()) {
            // No flags: read the urandom source, waiting only until the kernel's pool has been initialised once.
            const std::span<std::byte> rest = out.subspan(filled);
            const ssize_t count             = ::getrandom(rest.data(), rest.size(), 0);
            if (count < 0) {
                const int error = errno;
                if (error == EINTR) {
                    continue;
                }
                throw std::system_error{error, std::generic_category(), "getrandom"};
            }
            filled += static_cast<std::size_t>(count);
        }
    }

}  // namespace aloe::core
