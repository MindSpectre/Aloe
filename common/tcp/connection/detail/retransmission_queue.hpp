#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/wire>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <detail/node_pool.hpp>
#include <detail/sent_segment_pool.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's retransmission queue: every segment that occupies sequence space, SYN, SYN-ACK, data or
     * FIN, oldest first, from its first transmission until an ACK covers it; what the ACKs delivered; and the
     * scoreboard on its entries.
     *
     * The entries are nodes of the shard's pool, passed in. Once the connection is synchronized one node is set aside
     * for its FIN (`reserve_fin`), so a close never finds the pool empty, as Linux's FIN takes memory its accounting
     * cannot refuse; the end of a connection that never closed gives it back (`drop`).
     *
     * The scoreboard is two marks on the entries, reported received by a SACK block and lost, and the totals the rest
     * reads: the bytes and the segments reported received, the bytes lost and not resent since, and the highest
     * sequence the peer acknowledged either way. The byte totals count what no cumulative ACK has covered of each
     * marked entry: all of it, but for the acknowledged front of a head acknowledged in part, which SND.NXT - SND.UNA
     * no longer counts either. Every mark is set and cleared here, and every change to an entry's length goes through
     * here and takes off only an acknowledged front, so the totals stay those sums. An entry reported received keeps
     * its packet and its place until a cumulative ACK frees it.
     */
    template <device::IsPacket Packet>
    class RetransmissionQueue {
    public:
        using Pool  = SentSegmentPool<Packet>;
        using Entry = SentSegment<Packet>;

        /// What one ACK freed from the head: whole entries only.
        struct Freed {
            std::uint32_t length = 0;      ///< Sequence space freed.
            bool resent          = false;  ///< Some entry freed left other than exactly once: no sample from it (Karn).
            core::TimePoint sent{};        ///< The oldest entry freed: its last transmission.
            core::TimePoint newest_sent{};            ///< The newest entry freed: its last transmission.
            std::uint64_t delivered_at_send = 0;      ///< The newest entry freed: the delivered count when it was sent.
            bool app_limited                = false;  ///< The newest entry freed: its send left window unused.
        };

        /// What one block did to the entries.
        struct Reported {
            std::uint32_t bytes = 0;  ///< Sequence space newly marked received.
            bool reordering = false;  ///< It newly reported a never-resent entry below the highest already reported.
        };

        [[nodiscard]] bool empty() const noexcept {
            return chain_.empty();
        }

        [[nodiscard]] std::uint32_t size() const noexcept {
            return chain_.count;
        }

        /// The oldest entry's node; `no_node` when empty.
        [[nodiscard]] std::uint32_t head() const noexcept {
            return chain_.head;
        }

        /// The newest entry's node; `no_node` when empty.
        [[nodiscard]] std::uint32_t tail() const noexcept {
            return chain_.tail;
        }

        /// The entry at `position`, 0 the head.
        [[nodiscard]] const Entry& at(const Pool& pool, const std::size_t position) const noexcept {
            assert(position < chain_.count && "past the tail of the retransmission queue");
            std::uint32_t index = chain_.head;
            for (std::size_t step = 0; step < position; ++step) {
                index = pool.node(index).next;
            }
            return pool.node(index);
        }

        /// A segment the peer can acknowledge is out: the head left at least once. A head that never left, held for the
        /// peer's window or the next hop, has nothing in flight; entries leave in order, so no later one left before
        /// it.
        [[nodiscard]] bool in_flight(const Pool& pool) const noexcept {
            return !chain_.empty() && pool.node(chain_.head).transmits > 0;
        }

        /// `entry` on a free node at the tail; nothing, with `entry` untouched, when no node is free.
        [[nodiscard]] std::optional<std::uint32_t> push(Pool& pool, Entry&& entry) noexcept {
            return link(pool, pool.acquire(), std::move(entry));
        }

        /// The FIN's entry, on the node set aside for it, which spends the reservation; with none set aside, which the
        /// stack rules out, any free node.
        [[nodiscard]] std::optional<std::uint32_t> push_fin(Pool& pool, Entry&& entry) noexcept {
            assert(fin_reserved_ && "a FIN without the node set aside for it");
            return link(
                pool, std::exchange(fin_reserved_, false) ? pool.acquire_reserved() : pool.acquire(), std::move(entry));
        }

        /// Sets a node aside for the FIN; false when none is free.
        [[nodiscard]] bool reserve_fin(Pool& pool) noexcept {
            fin_reserved_ = pool.reserve();
            return fin_reserved_;
        }

        /// Takes back the tail `push` just linked; `previous` is the tail before it. Its packet goes with it.
        void pop_back(Pool& pool, const std::uint32_t previous) noexcept {
            pool.release_back(chain_, previous);
        }

        /**
         * Frees every entry from the head that `ack` covers whole and returns what it freed; an entry covered in part
         * stays whole, trimmed only if it is resent. Each entry it reaches is reconciled first, so a copy that came
         * back leaves its range. `each(entry)` runs for every entry it frees that no block reported, before the free.
         * The totals lose what `ack` covers of the marked entries: the whole of those it frees, the front of one it
         * covers in part. What `ack` covers past `una`, less the SYN and FIN of the entries freed and less what blocks
         * already reported, counts as delivered, a part of an entry included. The packets of a freed entry go back now
         * or when the device returns them. Called before SND.UNA moves.
         */
        template <std::invocable<const Entry&> Each>
        [[nodiscard]] Freed
        acknowledge(Pool& pool, const wire::TcpSequence ack, const wire::TcpSequence una, Each&& each) noexcept {
            Freed freed{};
            std::uint32_t control  = 0;  // the SYN and FIN sequence space of the entries freed
            std::uint32_t reported = 0;  // the payload `ack` covers of entries a block reported: delivered already
            while (!chain_.empty()) {
                reconcile(pool, chain_.head);
                Entry& entry                = pool.node(chain_.head);
                const wire::TcpSequence end = entry.first + entry.length;
                if (end.after(ack)) {
                    if ((entry.reported || entry.lost) && ack.after(entry.first)) {  // marked data, covered in part
                        const auto covered = static_cast<std::uint32_t>(ack - later(entry.first, una));
                        if (entry.reported) {
                            reported        += covered;
                            reported_bytes_ -= covered;
                        }
                        if (entry.lost) {
                            lost_bytes_ -= covered;
                        }
                    }
                    break;
                }
                const std::uint32_t remaining = outstanding(entry, una);
                if (entry.reported) {
                    reported        += remaining - (entry.length - entry.payload);
                    reported_bytes_ -= remaining;
                    --reported_segments_;
                } else {
                    each(std::as_const(entry));
                }
                if (entry.lost) {
                    lost_bytes_ -= remaining;
                }
                if (freed.length == 0) {
                    freed.sent = entry.sent;  // the oldest entry freed
                }
                freed.length            += entry.length;
                freed.resent             = freed.resent || entry.transmits != 1;
                freed.newest_sent        = entry.sent;
                freed.delivered_at_send  = entry.delivered_at_send;
                freed.app_limited        = entry.app_limited;
                control                 += entry.length - entry.payload;
                pool.release_front(chain_);
            }
            if (const auto covered = static_cast<std::uint32_t>(ack - una); covered > control + reported) {
                delivered_ += covered - control - reported;
            }
            acknowledged_ = ack;
            return freed;
        }

        /// A transmission of `entry` left: stamped, and on the queue too when it carries payload. A resend, the second
        /// transmission or later, is counted for `duplicate_report`.
        void sent(Entry& entry, const core::TimePoint now) noexcept {
            entry.stamp(now);
            if (entry.resent()) {
                ++resent_segments_;
            }
            if (entry.payload > 0) {
                last_data_send_ = now;
            }
        }

        /**
         * Marks every entry `block` covers whole as received, and calls `each(entry)` for every entry newly marked,
         * before the mark. An entry covered in part is left alone, as Linux's `tcp_match_skb_to_sack` leaves a buffer
         * of one segment; so is one that never left, which the peer cannot hold. An entry marked received stops being
         * lost, and its payload counts as delivered, once. The caller judged the block valid (`AckProcessor`).
         */
        template <std::invocable<const Entry&> Each>
        [[nodiscard]] Reported report(Pool& pool, const wire::TcpSackBlock& block, Each&& each) noexcept {
            Reported reported{};
            const std::optional<wire::TcpSequence> highest = highest_reported();
            for (std::uint32_t index = chain_.head; index != no_node;) {
                Entry& entry = pool.node(index);
                index        = entry.next;
                if (!entry.first.before(block.end)) {
                    break;  // oldest first: no later entry starts inside the block
                }
                const wire::TcpSequence end = entry.first + entry.length;
                if (entry.reported || entry.transmits == 0 || entry.first.before(block.first) || end.after(block.end)) {
                    continue;
                }
                each(std::as_const(entry));
                // Linux's `tcp_sacktag_one`: an original that fills a hole below the highest reported was reordered.
                reported.reordering =
                    reported.reordering || (!entry.resent() && highest && entry.first.before(*highest));
                const std::uint32_t remaining = outstanding(entry, acknowledged_);
                if (std::exchange(entry.lost, false)) {
                    lost_bytes_ -= remaining;
                }
                entry.reported   = true;
                reported_edge_   = reported_segments_ == 0 ? end : later(reported_edge_, end);
                reported_bytes_ += remaining;
                ++reported_segments_;
                delivered_     += entry.payload;
                reported.bytes += remaining;
            }
            return reported;
        }

        /**
         * RFC 2883 as Linux reads it (`tcp_check_dsack`, `tcp_dsack_seen`): `first` is a duplicate report when it
         * starts below `ack`, or `second` contains it. Not believed, and false, when it is longer than `max_window` or
         * when the peer has by now reported more duplicate segments than this queue ever resent. Every report within
         * `max_window` counts toward what the peer reported, believed or not, as `tcp_dsack_seen` counts it: one
         * segment, or its length in segments of `mss` rounded up. A reversed one is neither counted nor believed. It
         * reads the option alone, so it holds with nothing outstanding.
         */
        [[nodiscard]] bool duplicate_report(const wire::TcpSackBlock& first,
                                            const std::optional<wire::TcpSackBlock>& second,
                                            const wire::TcpSequence ack,
                                            const std::uint32_t max_window,
                                            const std::uint16_t mss) noexcept {
            const bool contained = second && !first.first.before(second->first) && !first.end.after(second->end);
            if ((!first.first.before(ack) && !contained) || !first.first.before(first.end)) {
                return false;
            }
            const auto length = static_cast<std::uint32_t>(first.end - first.first);
            if (length > max_window) {
                return false;
            }
            duplicate_segments_ += length > mss && mss > 0 ? (length + mss - 1) / mss : 1;
            return duplicate_segments_ <= resent_segments_;
        }

        /// `block` names, whole, a queued entry that was resent and is marked received.
        [[nodiscard]] bool names_reported_resend(const Pool& pool, const wire::TcpSackBlock& block) const noexcept {
            for (std::uint32_t index = chain_.head; index != no_node;) {
                const Entry& entry = pool.node(index);
                index              = entry.next;
                if (!entry.first.before(block.end)) {
                    return false;
                }
                if (entry.reported && entry.resent() && !entry.first.before(block.first) &&
                    !(entry.first + entry.length).after(block.end)) {
                    return true;
                }
            }
            return false;
        }

        /// Nothing for an entry reported or already lost.
        void mark_lost(Pool& pool, const std::uint32_t index) noexcept {
            Entry& entry = pool.node(index);
            if (entry.reported || entry.lost) {
                return;
            }
            entry.lost   = true;
            lost_bytes_ += outstanding(entry, acknowledged_);
        }

        /// A resend of it left.
        void clear_lost(Pool& pool, const std::uint32_t index) noexcept {
            if (Entry& entry = pool.node(index); std::exchange(entry.lost, false)) {
                lost_bytes_ -= outstanding(entry, acknowledged_);
            }
        }

        /// `SentSegment::reconcile` on the entry at `index`. A copy that came back first shrinks the entry to the
        /// copy's range, by a front a cumulative ACK had covered when the copy left; the totals count none of it.
        void reconcile(Pool& pool, const std::uint32_t index) noexcept {
            Entry& entry                                   = pool.node(index);
            [[maybe_unused]] const wire::TcpSequence first = entry.first;
            entry.reconcile();
            assert((entry.first == first || (acknowledged_ && !entry.first.after(*acknowledged_))) &&
                   "a copy starts at or below SND.UNA");
        }

        /// `SentSegment::trim_to` on the entry at `index`: the front `una` acknowledged goes, which the totals count
        /// none of.
        void trim_to(Pool& pool, const std::uint32_t index, const wire::TcpSequence una) noexcept {
            assert((acknowledged_ && !una.after(*acknowledged_)) && "only what a cumulative ACK covered is trimmed");
            pool.node(index).trim_to(una);
        }

        /// An undo.
        void clear_all_lost(Pool& pool) noexcept {
            for (std::uint32_t index = chain_.head; index != no_node; index = pool.node(index).next) {
                pool.node(index).lost = false;
            }
            lost_bytes_ = 0;
        }

        /// Reneging: the peer discarded what it reported. The highest sequence is the cumulative ACK's again.
        void clear_all_reported(Pool& pool) noexcept {
            for (std::uint32_t index = chain_.head; index != no_node; index = pool.node(index).next) {
                pool.node(index).reported = false;
            }
            reported_bytes_    = 0;
            reported_segments_ = 0;
        }

        /// The lowest entry marked lost; `no_node` when none.
        [[nodiscard]] std::uint32_t first_lost(const Pool& pool) const noexcept {
            for (std::uint32_t index = chain_.head; index != no_node; index = pool.node(index).next) {
                if (pool.node(index).lost) {
                    return index;
                }
            }
            return no_node;
        }

        /// The newest entry that was transmitted: the tail, or the entry before a FIN that waits for the window.
        [[nodiscard]] std::uint32_t last_sent(const Pool& pool) const noexcept {
            std::uint32_t last = no_node;
            for (std::uint32_t index = chain_.head; index != no_node; index = pool.node(index).next) {
                if (pool.node(index).transmits > 0) {
                    last = index;
                }
            }
            return last;
        }

        [[nodiscard]] bool head_reported(const Pool& pool) const noexcept {
            return !chain_.empty() && pool.node(chain_.head).reported;
        }

        [[nodiscard]] std::uint32_t reported_bytes() const noexcept {
            return reported_bytes_;
        }

        [[nodiscard]] std::uint32_t reported_segments() const noexcept {
            return reported_segments_;
        }

        [[nodiscard]] std::uint32_t lost_bytes() const noexcept {
            return lost_bytes_;
        }

        /// The highest sequence the peer acknowledged either way; nothing before a report or an ACK. While no entry is
        /// reported received it is the last cumulative ACK, as Linux's `tcp_highest_sack_seq` falls back to SND.UNA.
        [[nodiscard]] std::optional<wire::TcpSequence> highest_reported() const noexcept {
            return reported_segments_ > 0 ? std::optional{reported_edge_} : acknowledged_;
        }

        /// What is in flight of `flight`: less what is reported received, less what is lost and not resent. The totals
        /// count nothing a cumulative ACK covered, so they never pass what `flight` counts of the marked entries; the
        /// floor at zero only guards a `flight` that is not SND.NXT - SND.UNA.
        [[nodiscard]] std::uint32_t in_flight_bytes(const std::uint32_t flight) const noexcept {
            const std::uint32_t out = reported_bytes_ + lost_bytes_;
            return flight > out ? flight - out : 0;
        }

        /// RFC 5681's idle test for a send into the empty queue: the last data left more than `rto` ago.
        [[nodiscard]] bool idle(const core::TimePoint now, const core::Duration rto) const noexcept {
            return chain_.empty() && last_data_send_ && now - *last_data_send_ > rto;
        }

        /// Payload bytes the peer acknowledged, each once: when a block first reported it, or when a cumulative ACK
        /// first covered it, less SYN and FIN.
        [[nodiscard]] std::uint64_t delivered() const noexcept {
            return delivered_;
        }

        /// Every entry back to the pool: the node now, its packet and a copy out now or when the device returns them,
        /// each exactly once; the node set aside for a FIN that never came, any node's again; and the totals of the
        /// marks cleared.
        void drop(Pool& pool) noexcept {
            pool.release_all(chain_);
            if (std::exchange(fin_reserved_, false)) {
                pool.unreserve();
            }
            reported_bytes_    = 0;
            reported_segments_ = 0;
            lost_bytes_        = 0;
        }

    private:
        /// The later of two sequence numbers, in serial arithmetic.
        [[nodiscard]] static wire::TcpSequence later(const wire::TcpSequence one,
                                                     const wire::TcpSequence other) noexcept {
            return one.after(other) ? one : other;
        }

        /// What of `entry` no cumulative ACK has covered, with `una` the last one: all of it, but for the acknowledged
        /// front of a head acknowledged in part.
        [[nodiscard]] static std::uint32_t outstanding(const Entry& entry,
                                                       const std::optional<wire::TcpSequence> una) noexcept {
            return una && una->after(entry.first) ? static_cast<std::uint32_t>(entry.first + entry.length - *una)
                                                  : entry.length;
        }

        [[nodiscard]] std::optional<std::uint32_t>
        link(Pool& pool, const std::optional<std::uint32_t> index, Entry&& entry) noexcept {
            if (!index) {
                return std::nullopt;
            }
            Entry& node = pool.node(*index);
            node        = std::move(entry);
            node.next   = no_node;
            pool.push_back(chain_, *index);
            return index;
        }

        NodeChain chain_{};
        bool fin_reserved_       = false;  ///< A node of the pool is set aside for the FIN.
        std::uint64_t delivered_ = 0;
        std::optional<core::TimePoint> last_data_send_{};  ///< The last transmission with payload; nothing before one.
        // The scoreboard's totals: the sums over the marked entries of what no cumulative ACK covered.
        std::uint32_t reported_bytes_    = 0;
        std::uint32_t reported_segments_ = 0;
        std::uint32_t lost_bytes_        = 0;
        wire::TcpSequence reported_edge_{};  ///< The end of the highest entry reported; meaningful while one is.
        std::optional<wire::TcpSequence> acknowledged_{};  ///< The last cumulative ACK; nothing before one.
        // The duplicate report's belief, Linux's `total_retrans` and `dsack_dups`.
        std::uint64_t resent_segments_    = 0;  ///< Transmissions after an entry's first, of every entry ever queued.
        std::uint64_t duplicate_segments_ = 0;  ///< Segments the peer reported as duplicates, within `max_window`.
    };

}  // namespace aloe::tcp::detail
