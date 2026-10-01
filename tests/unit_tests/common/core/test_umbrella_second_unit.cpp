#include <aloe/core>
#include <string_view>

namespace aloe::testing {

    const std::string_view* version_object_seen_by_second_unit() noexcept {
        return &aloe::core::version_string;
    }

}  // namespace aloe::testing
