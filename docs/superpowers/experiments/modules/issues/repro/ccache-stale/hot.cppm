module;
#include <cstdint>
export module probe.hot;

export namespace probe::hot {
    [[nodiscard]] std::uint32_t mix_free(const std::uint32_t v) noexcept {
        return (v * 2654435761U) ^ (v >> 13U);
    }
    [[nodiscard]] inline std::uint32_t mix_inline(const std::uint32_t v) noexcept {
        return (v * 2654435761U) ^ (v >> 13U);
    }
    struct Mixer {
        [[nodiscard]] inline std::uint32_t mix_member(const std::uint32_t v) const noexcept {  // NOLINT(readability-convert-member-functions-to-static)
            return (v * 2654435761U) ^ (v >> 13U);
        }
    };
}  // namespace probe::hot
