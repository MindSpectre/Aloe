#pragma once

#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief RFC 5681's acknowledgement policy for in-order data: delayed to the next flush, except for every second
     * full-sized segment and for the data that fills a gap.
     */
    class AckPolicy {
    public:
        /// When the data just accepted is to be acknowledged.
        enum class Ack : std::uint8_t {
            Delayed,  ///< By the next flush, unless a data segment carries it first.
            Now,      ///< Before the next segment is processed.
        };

        /// Data was dropped above a gap, up to `end`: until the receive edge reaches the furthest such end, every
        /// segment is acknowledged at once.
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
            return Ack::Delayed;
        }

        /// A segment carrying an ACK left: the count of full-sized segments starts again.
        void acknowledged() noexcept {
            full_segments_ = 0;
        }

    private:
        std::optional<wire::TcpSequence> gap_end_{};  ///< The furthest end of data dropped above a gap.
        std::uint8_t full_segments_ = 0;              ///< Full-sized segments since the last ACK; saturates at two.
    };

}  // namespace aloe::tcp::detail
