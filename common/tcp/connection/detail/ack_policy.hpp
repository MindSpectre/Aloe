#pragma once

#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief RFC 5681's acknowledgement policy for in-order data: delayed to the next flush, except for every second
     * full-sized segment and for the data that fills a gap; and for segments above a gap, the count of those answered
     * at once since RCV.NXT last moved.
     */
    class AckPolicy {
    public:
        /// When the data just accepted is to be acknowledged.
        enum class Ack : std::uint8_t {
            Flush,  ///< By the next flush, unless a segment with an ACK leaves first.
            Now,    ///< Before the next segment is processed.
        };

        /// Data above a gap was refused, or released for room, up to `end`: until the receive edge reaches the
        /// furthest such end, every segment is acknowledged at once.
        void gap(const wire::TcpSequence end) noexcept {
            if (!gap_end_ || end.after(*gap_end_)) {
                gap_end_ = end;
            }
        }

        /**
         * In-order data of `accepted` bytes moved the receive edge to `next`. Filling a gap is acknowledged at once,
         * until the edge passes the last gap's end; so is the second of two full-sized segments, `full_size` being our
         * MSS less the timestamp option a timestamped peer puts in every segment.
         */
        [[nodiscard]] Ack
        on_data(const std::size_t accepted, const std::size_t full_size, const wire::TcpSequence next) noexcept {
            if (gap_end_) {
                if (!next.before(*gap_end_)) {
                    gap_end_.reset();
                }
                return Ack::Now;
            }
            if (accepted >= full_size) {
                if (full_segments_ < 2) {
                    ++full_segments_;
                }
                if (full_segments_ >= 2) {
                    return Ack::Now;  // a refused send leaves the count saturated at two
                }
            }
            return Ack::Flush;
        }

        /// A segment above a gap was held with RCV.NXT at `next`. Without SACK every one is answered at once; with it
        /// the first three since RCV.NXT last moved, and the rest by the flush (Linux, `__tcp_ack_snd_check`).
        [[nodiscard]] Ack on_out_of_order(const wire::TcpSequence next, const bool sack) noexcept {
            if (!sack) {
                return Ack::Now;
            }
            if (next != out_of_order_next_) {
                out_of_order_next_ = next;
                out_of_order_now_  = 0;
            }
            if (out_of_order_now_ < out_of_order_at_once) {
                ++out_of_order_now_;
                return Ack::Now;
            }
            return Ack::Flush;
        }

        /// A segment carrying an ACK left: the count of full-sized segments starts again.
        void acknowledged() noexcept {
            full_segments_ = 0;
        }

    private:
        /// Linux's `TCP_FASTRETRANS_THRESH`: three duplicates start the peer's fast retransmit.
        static constexpr std::uint8_t out_of_order_at_once = 3;

        std::optional<wire::TcpSequence> gap_end_{};  ///< The furthest end of data refused or released above a gap.
        std::uint8_t full_segments_ = 0;              ///< Full-sized segments since the last ACK; saturates at two.
        wire::TcpSequence out_of_order_next_{};       ///< The RCV.NXT the count of segments above a gap is for.
        std::uint8_t out_of_order_now_ = 0;           ///< Of them, those answered at once; at most three.
    };

}  // namespace aloe::tcp::detail
