#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace aloe::fabric {

    /// What happens to one frame as it leaves a port. A drop still counts as transmitted.
    enum class Action : std::uint8_t {
        Deliver,    ///< Delivered at once.
        Drop,       ///< Never delivered; the port still counts it as transmitted.
        Duplicate,  ///< Delivered twice in a row.
        Reorder,    ///< Held until the port's next frame has been delivered or the next `Fabric::advance`.
        Delay,      ///< Delivered by the first `Fabric::advance` at or past the fabric's current stamp plus `delay`.
    };

    /// A script's answer for one frame; `delay` is read only for `Action::Delay`.
    struct Verdict {
        Action action = Action::Deliver;
        core::Duration delay{};
    };

    /// One frame as the script sees it: its position in the port's egress, counted from zero, and its bytes.
    struct Egress {
        std::uint64_t index = 0;
        std::span<const std::byte> frame;
    };

    /// A port's egress impairment. The script's verdict wins; with none, the model draws in the order loss,
    /// duplicate, reorder, delay, from an engine seeded with `seed`.
    struct Impairment {
        double loss      = 0;     ///< Probability that a frame is dropped.
        double duplicate = 0;     ///< Probability that a frame is delivered twice.
        double reorder   = 0;     ///< Probability that a frame is held behind the next one.
        core::Duration delay{};   ///< Fixed delay from the fabric's current stamp.
        core::Duration jitter{};  ///< Up to this much more delay, drawn per frame.
        std::uint64_t seed = 1;   ///< Seeds the engine; the same seed gives the same run.
        std::function<std::optional<Verdict>(const Egress&)> script = {};  ///< Nullopt leaves the frame to the model.
    };

    struct ImpairmentCounters {
        std::uint64_t dropped    = 0;
        std::uint64_t duplicated = 0;
        std::uint64_t reordered  = 0;
        std::uint64_t delayed    = 0;

        friend constexpr bool operator==(const ImpairmentCounters&, const ImpairmentCounters&) noexcept = default;
    };

}  // namespace aloe::fabric
