#pragma once

#include <aloe/core>
#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace aloe::tcp {

    /// What an ACK of new data tells the congestion control; later models read more fields, the concept stays.
    struct AckInfo {
        std::uint32_t acked  = 0;  ///< Bytes of sequence space newly acknowledged.
        std::uint32_t flight = 0;  ///< Bytes in flight before this ACK.
        std::uint16_t mss    = 0;
        std::optional<core::Duration> rtt{};  ///< The round-trip sample, when one was taken.
        core::TimePoint now{};
        core::TimePoint sent{};                   ///< The last transmission of the newest entry this ACK freed.
        std::uint64_t delivered         = 0;      ///< The connection's delivered bytes after this ACK.
        std::uint64_t delivered_at_send = 0;      ///< The connection's delivered bytes when that entry was sent.
        bool app_limited                = false;  ///< That send left window unused.
    };

    /**
     * A congestion control: stateless, with a trivially copyable per-connection `State` the connection embeds, and
     * copies when an episode of loss begins, for the undo.
     *
     * The policy answers how much may be in flight, `window`, and is told what happened. It sets a target when a loss
     * is found, RFC 5681's `ssthresh`; the way from its window down to that target, while the loss is repaired, is
     * the connection's (RFC 9937's rate reduction), which hands the window back at the target with `on_recovery_end`.
     * A policy whose target is the largest value has nothing to reduce: its window stands through recovery.
     */
    template <typename P>
    concept IsCongestionControl =
        std::default_initializable<typename P::State> && std::is_trivially_copyable_v<typename P::State> &&
        requires(typename P::State& state,
                 const typename P::State& const_state,
                 const AckInfo& info,
                 std::uint32_t flight,
                 std::uint32_t in_flight,
                 std::uint16_t mss) {
            // At Established: the initial window.
            { P::reset(state, mss) } -> std::same_as<void>;
            // The congestion window in bytes.
            { P::window(const_state) } -> std::same_as<std::uint32_t>;
            // RFC 5681's ssthresh; the largest value when unbounded.
            { P::target(const_state) } -> std::same_as<std::uint32_t>;
            // An ACK of new data, open or in the loss state.
            { P::on_ack(state, info) } -> std::same_as<void>;
            // A loss was found, or a timeout is due a new reduction: the policy sets its target.
            { P::on_congestion(state, flight, mss) } -> std::same_as<void>;
            // Recovery ended: the window becomes the target.
            { P::on_recovery_end(state) } -> std::same_as<void>;
            // The RTO expired: the window restarts at `in_flight` plus one segment.
            { P::on_timeout(state, in_flight, mss) } -> std::same_as<void>;
            // A send after an idle longer than the RTO.
            { P::on_idle(state, mss) } -> std::same_as<void>;
            // A reduction was needless: the second is the state copied at the episode's start.
            { P::on_undo(state, const_state) } -> std::same_as<void>;
        };

}  // namespace aloe::tcp
