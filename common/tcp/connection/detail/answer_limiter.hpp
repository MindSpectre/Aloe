#pragma once

#include <aloe/core>
#include <optional>

namespace aloe::tcp::detail {

    /// When the connection last sent an answer that goes through the limit (RFC 5961 section 7, Linux's
    /// `last_oow_ack_time`): one stamp, shared by every kind of limited answer.
    ///
    /// RFC 5961's throttle counts answers per window of seconds; this is Linux's per-connection rule instead
    /// (`__tcp_oow_rate_limited`): one answer per interval, so a challenge holds back the answer to an empty
    /// out-of-window segment and the other way round. A fresh slot holds no stamp.
    class AnswerLimiter {
    public:
        /// True when an answer may go at `now`, which becomes the stamp; false within `interval` of the last one. A
        /// zero interval always allows.
        [[nodiscard]] bool allow(const core::TimePoint now, const core::Duration interval) noexcept {
            if (interval == core::Duration::zero()) {
                return true;
            }
            if (last_ && now - *last_ < interval) {
                return false;
            }
            last_ = now;
            return true;
        }

    private:
        std::optional<core::TimePoint> last_{};  ///< The last answer allowed; none before the first.
    };

}  // namespace aloe::tcp::detail
