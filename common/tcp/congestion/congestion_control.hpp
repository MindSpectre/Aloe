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

    /// A congestion control: stateless, with a trivially copyable per-connection `State` the connection embeds.
    template <typename P>
    concept IsCongestionControl =
        std::default_initializable<typename P::State> && std::is_trivially_copyable_v<typename P::State> &&
        requires(typename P::State& state,
                 const typename P::State& const_state,
                 const AckInfo& info,
                 std::uint32_t flight,
                 std::uint32_t acked,
                 std::uint16_t mss) {
            { P::reset(state, mss) } -> std::same_as<void>;             // at Established: the initial window
            { P::window(const_state) } -> std::same_as<std::uint32_t>;  // the congestion window in bytes
            { P::on_ack(state, info) } -> std::same_as<void>;           // new data acknowledged outside recovery
            { P::on_loss(state, flight, mss) } -> std::same_as<void>;   // the third duplicate ACK
            { P::on_dupack(state, mss) } -> std::same_as<void>;         // a further duplicate in recovery
            { P::on_partial_ack(state, acked, mss) } -> std::same_as<void>;
            { P::on_recovery_end(state, flight, mss) } -> std::same_as<void>;
            { P::on_timeout(state, flight, mss) } -> std::same_as<void>;  // the first expiry of an episode
            { P::on_idle(state, mss) } -> std::same_as<void>;             // a send after an idle longer than the RTO
        };

}  // namespace aloe::tcp
