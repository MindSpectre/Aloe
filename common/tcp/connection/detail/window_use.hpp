#pragma once

#include <aloe/wire>
#include <cstdint>

namespace aloe::tcp::detail {

    /**
     * @brief Whether the congestion window is in use, as the sends found it: what Linux's `tcp_cwnd_validate` keeps,
     * in bytes, for the policy to grow the window only while it limits what is sent (`tcp_is_cwnd_limited`).
     *
     * A round runs from a record to the ACK of the last byte sent by then. Within it a record stands until a send the
     * window limited replaces it, or, while none was limited, one that leaves more in flight than any before; the first
     * send after the round ended starts the next whatever it carries. So `limited` says whether a send of the round
     * left less than a segment of the window or was held back by it, and `largest_flight` is what was in flight at
     * the send recorded: the most of the round while none was limited. A send is of new data, recorded as it leaves
     * and when the window holds one back before it leaves; a resend leaves the record as it is, as Linux's resends
     * never reach `tcp_cwnd_validate`.
     */
    class WindowUse {
    public:
        /// Linux's `tcp_cwnd_validate` at a send: `in_flight` after it, `limited` when what is in flight reached the
        /// window, in segments (less than one left), or the window held the send back. Recorded when a round ended
        /// (`una` reached the sequence stored), or the send was limited, or it was not and `in_flight` passed the
        /// largest stored.
        void on_send(const wire::TcpSequence una,
                     const wire::TcpSequence nxt,
                     const std::uint32_t in_flight,
                     const bool limited) noexcept {
            if (!una.before(round_end_) || limited || (!limited_ && in_flight > largest_flight_)) {
                limited_        = limited;
                largest_flight_ = in_flight;
                round_end_      = nxt;
            }
        }

        /// The window limited the send recorded last.
        [[nodiscard]] bool limited() const noexcept {
            return limited_;
        }

        /// What was in flight at the send recorded last: the most of its round, unless that send was limited.
        [[nodiscard]] std::uint32_t largest_flight() const noexcept {
            return largest_flight_;
        }

    private:
        wire::TcpSequence round_end_{};  ///< SND.NXT at the last record: the round ends once SND.UNA reaches it.
        std::uint32_t largest_flight_ = 0;
        bool limited_                 = false;
    };

}  // namespace aloe::tcp::detail
