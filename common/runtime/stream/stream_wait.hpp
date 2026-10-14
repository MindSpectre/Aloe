#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <stream_events.hpp>
#include <work.hpp>

namespace aloe::runtime::detail {

    /// The kinds of wait a connection slot parks, one of each at most.
    enum class WaitKind : std::uint8_t { Readable, Writable, Acked, Connected, Closed, Accept };

    inline constexpr std::size_t slot_wait_kinds = 5;  ///< Accept parks on a listener, not a slot.

    /// What a wait completes with, saved on the node by the wake pass; the operation turns it into its value.
    struct WaitOutcome {
        std::optional<stream::Error> error{};
        std::size_t value = 0;  ///< Bytes, or the index of a connection to hand over.
        bool stopped      = false;
    };

    /**
     * @brief A parked or queued stream operation: a loop::Work the shard runs, with what the owner
     * needs to complete it from a wake pass without calling into the receiver.
     *
     * The derived operation state sets `run` (its completion) and `drop_callbacks` (resetting the
     * stop callbacks it registered). Phases: Parked on a slot's kind pointer and the slot's active
     * list; Queued once detached from the kind pointer and pushed onto the run queue, still on the
     * active list so a release can turn the outcome into stopped; Completed when run.
     */
    struct StreamWait : loop::Work {
        enum class Phase : std::uint8_t { Idle, Parked, Queued, Completed };
        using Drop = void (*)(StreamWait&) noexcept;

        StreamWait(const loop::Work::Function complete, const Drop drop) noexcept
            : loop::Work{complete},
              drop_callbacks{drop} {
        }

        Phase phase           = Phase::Idle;
        WaitKind kind         = WaitKind::Readable;
        std::uint32_t index   = 0;  ///< The connection slot; for Accept, the listener's port.
        std::size_t threshold = 0;  ///< Bytes for Readable and Writable, the sequence value for Acked.
        WaitOutcome outcome{};
        Drop drop_callbacks     = nullptr;
        StreamWait* next_active = nullptr;
        StreamWait* prev_active = nullptr;
        bool active             = false;  ///< On a slot's active list.
    };

}  // namespace aloe::runtime::detail
