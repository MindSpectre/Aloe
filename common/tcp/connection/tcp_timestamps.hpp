#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's RFC 7323 timestamps: the offer and then the negotiated option, the clock, TS.Recent with
     * PAWS, and Last.ACK.sent.
     *
     * In SynSent `enabled()` is the SYN's offer; from the handshake on it is the option both ends offered. The clock
     * is the stack's stamp in milliseconds, wrapping, plus an offset drawn per connection.
     */
    class Timestamps {
    public:
        /// RFC 7323's bound on TS.Recent: PAWS ignores a value older than 24 days, past which the peer's clock may
        /// have wrapped half its range at one tick per millisecond.
        static constexpr core::Duration paws_window = std::chrono::hours{24 * 24};

        /// The option the SYN or SYN-ACK offers, and the clock's offset.
        void offer(const bool enabled, const std::uint32_t offset) noexcept {
            enabled_ = enabled;
            offset_  = offset;
        }

        /// The peer's SYN or SYN-ACK answered the offer: the option stays on only when it carried one, and TS.Recent
        /// takes its value.
        void answer(const std::optional<std::uint32_t> value, const core::TimePoint now) noexcept {
            enabled_ = enabled_ && value.has_value();
            if (enabled_) {
                take(*value, now);
            }
        }

        /// The offer was never answered: nothing was negotiated.
        void withdraw() noexcept {
            enabled_ = false;
        }

        [[nodiscard]] bool enabled() const noexcept {
            return enabled_;
        }

        /// The bytes the option takes in every segment: nothing when it is off.
        [[nodiscard]] std::size_t option_size() const noexcept {
            return enabled_ ? wire::TcpOptions::timestamps_size : 0;
        }

        /// The connection's clock: the stamp's milliseconds, wrapping, plus its own offset.
        [[nodiscard]] std::uint32_t value(const core::TimePoint now) const noexcept {
            return static_cast<std::uint32_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) +
                   offset_;
        }

        /// TS.Recent: what the next segment echoes.
        [[nodiscard]] std::uint32_t echo() const noexcept {
            return recent_;
        }

        /**
         * RFC 7323's PAWS test: with the option on and a TS.Recent, a non-RST segment whose value is older than
         * TS.Recent, in the option's signed 32-bit arithmetic, is rejected, unless TS.Recent is older than 24 days,
         * past which the peer's clock may have wrapped. A segment without the option passes, as Linux lets it.
         */
        [[nodiscard]] bool
        rejects(const std::optional<std::uint32_t> value, const bool rst, const core::TimePoint now) const noexcept {
            return enabled_ && recent_valid_ && value && !rst && static_cast<std::int32_t>(*value - recent_) < 0 &&
                   now - recent_stamp_ < paws_window;
        }

        /**
         * RFC 7323's TS.Recent rule, for a segment that passed the sequence check, `arrived` its sequence number before
         * any trim: its value replaces TS.Recent when it is not older, or TS.Recent is past the PAWS window, and the
         * segment starts at or before Last.ACK.sent, so the echo is the value of the segment the last ACK sent
         * acknowledged, as Linux's `tcp_replace_ts_recent`.
         */
        void update(const std::optional<std::uint32_t> value,
                    const wire::TcpSequence arrived,
                    const core::TimePoint now) noexcept {
            if (!enabled_ || !value || arrived.after(last_ack_sent_)) {
                return;
            }
            if (static_cast<std::int32_t>(*value - recent_) >= 0 || now - recent_stamp_ >= paws_window) {
                take(*value, now);
            }
        }

        /// Last.ACK.sent: the acknowledgement the last ACK-bearing segment carried.
        void sent_ack(const wire::TcpSequence ack) noexcept {
            last_ack_sent_ = ack;
        }

        /// A round-trip sample from an ACK's echo, which names the transmission it answers; nothing with the option
        /// off, no echo, an echo of zero, or one from the future.
        [[nodiscard]] std::optional<core::Duration> sample(const std::optional<std::uint32_t> echo,
                                                           const core::TimePoint now) const noexcept {
            if (!enabled_ || !echo || *echo == 0) {
                return std::nullopt;
            }
            const auto elapsed = static_cast<std::uint32_t>(value(now) - *echo);
            if (static_cast<std::int32_t>(elapsed) < 0) {
                return std::nullopt;
            }
            return std::chrono::milliseconds{elapsed};
        }

    private:
        /// TS.Recent takes `value`, stamped now.
        void take(const std::uint32_t value, const core::TimePoint now) noexcept {
            recent_       = value;
            recent_stamp_ = now;
            recent_valid_ = true;
        }

        bool enabled_         = false;
        std::uint32_t offset_ = 0;
        std::uint32_t recent_ = 0;           ///< TS.Recent.
        core::TimePoint recent_stamp_{};     ///< When TS.Recent was last set; PAWS ignores it after 24 days.
        bool recent_valid_ = false;          ///< TS.Recent holds a value from the peer.
        wire::TcpSequence last_ack_sent_{};  ///< Last.ACK.sent.
    };

}  // namespace aloe::tcp::detail
