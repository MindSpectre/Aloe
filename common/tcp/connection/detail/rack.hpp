#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/device>
#include <aloe/wire>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>

#include <detail/node_pool.hpp>
#include <detail/retransmission_queue.hpp>
#include <detail/sent_segment_pool.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief RACK, RFC 8985 section 6: a connection's time-based loss detection, on a connection that uses SACK.
     *
     * It owns RACK's state: the transmit stamp and end sequence of the most recently sent segment the peer has
     * acknowledged either way, RACK's segment; the round trip of the last segment acknowledged; the minimum round trip,
     * kept over about five minutes; whether reordering was seen; and the reordering window's multiplier, the
     * recoveries it persists for, and the round in which a duplicate report last grew it. Detection walks the
     * retransmission queue and marks there what RACK finds lost; the queue owns the marks.
     *
     * Every entry sent before RACK's segment that is neither reported received nor waiting for its resend is lost once
     * `sent + round trip + window` has passed; an entry already resent is judged by its resend's stamp, so a resend
     * that is lost again is marked again. The queue is in sequence order, which for entries never resent is transmit
     * order, so the walk ends at the first of them that was not sent before RACK's segment: an in-order ACK with
     * nothing reported and nothing resent pays one comparison at the head.
     *
     * Stamps are the stack's stamps of its ticks, shared by every segment sent in a tick, so equal stamps are told
     * apart by sequence, as RFC 8985's `RACK_sent_after` breaks its ties. Sequence numbers compare in serial
     * arithmetic.
     */
    class Rack {
    public:
        /// What one detection did.
        struct Detected {
            std::uint32_t marked       = 0;  ///< Entries newly marked lost.
            std::uint32_t resends_lost = 0;  ///< Of them, resends a lost mark owed, marked again.
            core::Duration wait{};           ///< The longest wait left among entries not yet due; zero when none waits.
        };

        /**
         * A round-trip sample: the minimum over about five minutes, kept as two windows of 300 s. A sample at least
         * `min_window` after the current window began starts a new one, and the window before is kept, unless it is
         * older than two windows; the minimum is the smaller of the two. RFC 8985 6.2 step 1's windowed min filter,
         * which leaves the filter to the implementation: coarser than Linux's `minmax_running_min` over three
         * estimates, and like it updated only by samples. Linux's `tcp_update_rtt_min` also passes over a sample a
         * delayed ACK may have stretched, and keeps a sample of zero as one jiffy; neither is copied: every sample
         * counts, as it is.
         */
        void sample(const core::Duration round_trip, const core::TimePoint now) noexcept {
            if (!current_ || now - current_start_ >= min_window) {
                previous_      = current_ && now - current_start_ < 2 * min_window ? current_ : std::nullopt;
                current_       = round_trip;
                current_start_ = now;
                return;
            }
            current_ = std::min(*current_, round_trip);
        }

        /**
         * An entry was newly reported, by a block or by the cumulative ACK: `sent` its last transmission, `end` its
         * end sequence. Skipped when it was resent and `now - sent` is below the minimum round trip, or
         * `echo_before_resend` (a cumulative ACK whose echo is older than the resend). Otherwise RACK's round trip is
         * `now - sent`, and its stamp and end are taken when later, by stamp and then by sequence.
         *
         * RFC 8985 6.2 step 2 and Linux's `tcp_rack_advance`. Before the first sample there is no minimum, and a resend
         * is skipped, as Linux's minimum starts at its largest value: nothing yet says the ACK can be for the resend.
         */
        void advance(const core::TimePoint sent,
                     const wire::TcpSequence end,
                     const bool resent,
                     const bool echo_before_resend,
                     const core::TimePoint now) noexcept {
            const core::Duration round_trip = now - sent;
            if (resent && (echo_before_resend || !min_round_trip() || round_trip < *min_round_trip())) {
                return;
            }
            advanced_   = true;
            round_trip_ = round_trip;
            if (!segment_ || sent_after(sent, end, segment_->sent, segment_->end)) {
                segment_ = Segment{.sent = sent, .end = end};
            }
        }

        /// Reordering was seen: from now on the window is never zero for want of evidence.
        void see_reordering() noexcept {
            reordering_seen_ = true;
        }

        [[nodiscard]] bool reordering_seen() const noexcept {
            return reordering_seen_;
        }

        /**
         * RFC 8985's `RACK_update_reo_wnd`, the part an ACK drives: a round that began at `nxt` ends when `una`
         * reaches it; a believed duplicate report that is not a probe's, once reordering was seen and outside such a
         * round, grows the multiplier by one, starts a round at `nxt` and sets the persistence to 16.
         *
         * "Once reordering was seen" is Linux's (`tcp_dsack_seen` sets `rack.dsack_seen` only while `reord_seen`);
         * the RFC's pseudocode grows on any duplicate report. The caller passes the report before it records the
         * reordering the same ACK shows, as Linux reads `reord_seen` before its walk records any.
         */
        void on_ack(const wire::TcpSequence una, const wire::TcpSequence nxt, const bool duplicate_report) noexcept {
            if (round_ && !una.before(*round_)) {
                round_.reset();
            }
            if (!duplicate_report || !reordering_seen_ || round_) {
                return;
            }
            round_ = nxt;
            if (multiplier_ < std::numeric_limits<std::uint8_t>::max()) {
                ++multiplier_;
            }
            persist_ = persistence;
        }

        /// A recovery ended and was not undone: the persistence drops, and at zero the multiplier is one again.
        void recovery_ended() noexcept {
            if (persist_ > 0) {
                --persist_;
            }
            if (persist_ == 0) {
                multiplier_ = 1;
            }
        }

        /// An undo by duplicate SACK: the persistence rises by one, to at most 16.
        void undone_by_report() noexcept {
            if (persist_ < persistence) {
                ++persist_;
            }
        }

        /**
         * The reordering window: zero while reordering was never seen and the connection is `recovering`, in recovery
         * or in the loss state, or has three or more segments reported received; else a quarter of the minimum round
         * trip times the multiplier, never more than `srtt`. Zero with no round trip measured yet (RFC 8985 3.3.2:
         * "or zero if no round-trip time estimate is available"). Linux's `tcp_rack_reo_wnd`.
         */
        [[nodiscard]] core::Duration window(const bool recovering,
                                            const std::uint32_t reported_segments,
                                            const std::optional<core::Duration> srtt) const noexcept {
            if (!reordering_seen_ && (recovering || reported_segments >= duplicate_threshold)) {
                return core::Duration::zero();
            }
            const std::optional<core::Duration> least = min_round_trip();
            if (!least || !srtt) {
                return core::Duration::zero();
            }
            return std::min(*least / 4 * multiplier_, *srtt);
        }

        /// RACK's state moved since the last detection.
        [[nodiscard]] bool advanced() const noexcept {
            return advanced_;
        }

        /**
         * Marks every entry that is neither reported nor waiting for its resend, was sent before RACK's segment, and
         * whose time has passed: `sent + round trip + window <= now`. Walks from the head and stops at the first entry
         * that never was resent and was not sent before RACK's segment, and at the first that never left: entries
         * leave in order, and every entry behind such a one was last sent after it. An entry already resent is judged
         * by its resend's stamp. One whose resend a lost mark owed is marked again and counted among `resends_lost`, as
         * Linux's `tcp_mark_skb_lost` counts a `TCPCB_SACKED_RETRANS` segment; a segment a tail probe sent again was
         * never marked, and its loss is a first one, as Linux's probe sets only `TCPCB_EVER_RETRANS`
         * (`SentSegment::resend_out`). RFC 8985's `RACK_detect_loss`, Linux's `tcp_rack_detect_loss`, which walks its
         * list in transmit order where this walks the queue in sequence order. Clears `advanced()`.
         *
         * `window_of()` gives the reordering window, asked once and only when an entry needs it: an empty queue, or a
         * head never resent and not sent before RACK's segment, an in-order ACK's case, computes none.
         */
        template <device::IsPacket Packet, std::invocable<> WindowOf>
        [[nodiscard]] Detected detect(RetransmissionQueue<Packet>& queue,
                                      SentSegmentPool<Packet>& pool,
                                      WindowOf&& window_of,
                                      const core::TimePoint now) noexcept {
            advanced_ = false;
            Detected detected{};
            if (!segment_) {
                return detected;
            }
            std::optional<core::Duration> window{};
            for (std::uint32_t index = queue.head(); index != no_node; index = pool.node(index).next) {
                const SentSegment<Packet>& entry = pool.node(index);
                if (entry.transmits == 0) {
                    break;  // it never left, and nothing behind it did
                }
                const bool before = sent_after(segment_->sent, segment_->end, entry.sent, entry.first + entry.length);
                if (!before && !entry.resent()) {
                    break;  // every entry behind it was last sent after it
                }
                if (!before || entry.reported || entry.lost) {
                    continue;
                }
                if (!window) {
                    window = window_of();
                }
                if (const core::Duration remaining = entry.sent + round_trip_ + *window - now;
                    remaining > core::Duration::zero()) {
                    detected.wait = std::max(detected.wait, remaining);
                    continue;
                }
                if (entry.resend_out) {
                    ++detected.resends_lost;
                }
                queue.mark_lost(pool, index);
                ++detected.marked;
            }
            return detected;
        }

        /// `detect` with the reordering window already known.
        template <device::IsPacket Packet>
        [[nodiscard]] Detected detect(RetransmissionQueue<Packet>& queue,
                                      SentSegmentPool<Packet>& pool,
                                      const core::Duration window,
                                      const core::TimePoint now) noexcept {
            return detect(queue, pool, [window] noexcept { return window; }, now);
        }

        /// The round trip of the last segment acknowledged that RACK took; zero before one.
        [[nodiscard]] core::Duration round_trip() const noexcept {
            return round_trip_;
        }

        /// The minimum round trip over the last window or two; nothing before the first sample.
        [[nodiscard]] std::optional<core::Duration> min_round_trip() const noexcept {
            if (!current_) {
                return std::nullopt;
            }
            return previous_ ? std::min(*previous_, *current_) : *current_;
        }

    private:
        /// The minimum round trip is kept over two windows of this length: Linux's `tcp_min_rtt_wlen`, 300 s.
        static constexpr core::Duration min_window         = std::chrono::seconds{300};
        /// The recoveries a grown reordering window persists for: RFC 8985's 16, Linux's `TCP_RACK_RECOVERY_THRESH`.
        static constexpr std::uint8_t persistence          = 16;
        /// RFC 5681's DupThresh: with no reordering seen, three segments reported make the window zero.
        static constexpr std::uint32_t duplicate_threshold = 3;

        /// RACK's segment: the most recently sent segment acknowledged either way.
        struct Segment {
            core::TimePoint sent{};
            wire::TcpSequence end{};
        };

        /// RFC 8985's `RACK_sent_after` and Linux's `tcp_skb_sent_after`: a later stamp, or the same stamp and a higher
        /// end sequence.
        [[nodiscard]] static bool sent_after(const core::TimePoint one,
                                             const wire::TcpSequence one_end,
                                             const core::TimePoint other,
                                             const wire::TcpSequence other_end) noexcept {
            return one > other || (one == other && one_end.after(other_end));
        }

        // RACK's segment and the round trip, written by every ACK that reports or acknowledges anything.
        std::optional<Segment> segment_{};
        core::Duration round_trip_{};
        bool advanced_ = false;  ///< RACK's state moved since the last detection.
        // The minimum round trip: the current window's and the one before.
        std::optional<core::Duration> current_{};
        core::TimePoint current_start_{};
        std::optional<core::Duration> previous_{};
        // The reordering window's adaptation.
        bool reordering_seen_    = false;
        std::uint8_t multiplier_ = 1;
        std::uint8_t persist_    = 0;               ///< Recoveries left before the multiplier is one again.
        std::optional<wire::TcpSequence> round_{};  ///< SND.NXT when a duplicate report last grew the multiplier.
    };

}  // namespace aloe::tcp::detail
