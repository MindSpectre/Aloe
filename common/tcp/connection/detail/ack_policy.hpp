#pragma once

#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief RFC 5681's acknowledgement policy for in-order data: delayed to the next flush, or with a delay configured
     * to its deadline, except for the second full-sized segment, or two full segments' worth of bytes with the delay,
     * and for data below what is held above or refused; and for segments above a gap, the count of those answered at
     * once since RCV.NXT last moved, which leaves out an answer that also acknowledges in-order data.
     */
    class AckPolicy {
    public:
        /// When the data just accepted is to be acknowledged.
        enum class Ack : std::uint8_t {
            Flush,    ///< By the next flush, unless a segment with an ACK leaves first.
            Delayed,  ///< When the delay runs out, unless a segment with an ACK leaves first.
            Now,      ///< Before the next segment is processed.
        };

        /// Data above a gap was refused, or released for room, up to `end`: until the receive edge reaches the
        /// furthest such end, every segment is acknowledged at once.
        void gap(const wire::TcpSequence end) noexcept {
            if (!gap_end_ || end.after(*gap_end_)) {
                gap_end_ = end;
            }
        }

        /// In-order data of `accepted` bytes moved RCV.NXT to `next`. At once while a refused end is ahead, while
        /// anything is held above, and for the second full-sized segment; with `delay`, at once when two full segments'
        /// worth of bytes is unacknowledged and `Delayed` otherwise. `full_size` is the connection's `mss()`.
        [[nodiscard]] Ack on_data(const std::size_t accepted,
                                  const std::size_t full_size,
                                  const wire::TcpSequence next,
                                  const bool held_above,
                                  const bool delay) noexcept {
            data_owed_ = true;
            if (gap_end_) {
                if (!next.before(*gap_end_)) {
                    gap_end_.reset();
                }
                return Ack::Now;
            }
            if (held_above) {
                return Ack::Now;
            }
            if (delay) {
                // Counted in bytes, so short segments are acknowledged as often as full ones; a refused send leaves
                // the count past the mark, and every segment tries again until an ACK leaves.
                unacknowledged_ += accepted;
                return unacknowledged_ >= 2 * full_size ? Ack::Now : Ack::Delayed;
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
        /// the first three since RCV.NXT last moved, and the rest by the flush (Linux, `__tcp_ack_snd_check`). An
        /// answer while in-order data is still unacknowledged goes at once and is not one of the three: it moves the
        /// peer's SND.UNA, so the peer does not count it as a duplicate. Linux charges it anyway, which a sender that
        /// reads the blocks does not mind; Aloe departs, because a sender that counts duplicates needs three true ones.
        [[nodiscard]] Ack on_out_of_order(const wire::TcpSequence next, const bool sack) noexcept {
            if (!sack) {
                return Ack::Now;
            }
            if (next != out_of_order_next_) {
                out_of_order_next_ = next;
                out_of_order_now_  = 0;
            }
            if (data_owed_) {
                return Ack::Now;  // uncounted; refused, the data stays owed and the next answer is uncounted too
            }
            if (out_of_order_now_ < out_of_order_at_once) {
                ++out_of_order_now_;
                return Ack::Now;
            }
            return Ack::Flush;
        }

        /// A segment carrying an ACK left: the count of full-sized segments, and the bytes a delayed ACK covers, start
        /// again, and no in-order data is owed an ACK.
        void acknowledged() noexcept {
            full_segments_  = 0;
            unacknowledged_ = 0;
            data_owed_      = false;
        }

    private:
        /// Linux's `TCP_FASTRETRANS_THRESH`: three duplicates start the peer's fast retransmit.
        static constexpr std::uint8_t out_of_order_at_once = 3;

        std::optional<wire::TcpSequence> gap_end_{};  ///< The furthest end of data refused or released above a gap.
        std::uint8_t full_segments_ = 0;              ///< Full-sized segments since the last ACK; saturates at two.
        std::size_t unacknowledged_ = 0;              ///< With a delay: in-order bytes since the last ACK left.
        wire::TcpSequence out_of_order_next_{};       ///< The RCV.NXT the count of segments above a gap is for.
        std::uint8_t out_of_order_now_ = 0;           ///< Of them, those answered at once; at most three.
        bool data_owed_                = false;       ///< In-order data accepted since the last ACK left.
    };

}  // namespace aloe::tcp::detail
