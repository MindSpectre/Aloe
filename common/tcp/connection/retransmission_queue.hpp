#pragma once

#include <aloe/core>
#include <aloe/device>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <node_pool.hpp>
#include <sent_segment_pool.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's retransmission queue: every segment that occupies sequence space, SYN, SYN-ACK, data or
     * FIN, oldest first, from its first transmission until an ACK covers it; and what the ACKs delivered.
     *
     * The entries are nodes of the shard's pool, passed in. Once the connection is synchronized one node is set aside
     * for its FIN (`reserve_fin`), so a close never finds the pool empty, as Linux's FIN takes memory its accounting
     * cannot refuse; the end of a connection that never closed gives it back (`drop`).
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
         * back leaves its range. What `ack` covers past `una`, less the SYN and FIN of the entries freed, counts as
         * delivered, a part of an entry included. The packets of a freed entry go back now or when the device returns
         * them. Called before SND.UNA moves.
         */
        [[nodiscard]] Freed acknowledge(Pool& pool, const wire::TcpSequence ack, const wire::TcpSequence una) noexcept {
            Freed freed{};
            std::uint32_t control = 0;  // the SYN and FIN sequence space of the entries freed
            while (!chain_.empty()) {
                Entry& entry = pool.node(chain_.head);
                entry.reconcile();
                if ((entry.first + entry.length).after(ack)) {
                    break;
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
            if (const auto covered = static_cast<std::uint32_t>(ack - una); covered > control) {
                delivered_ += covered - control;
            }
            return freed;
        }

        /// A transmission of `entry` left: stamped, and on the queue too when it carries payload.
        void sent(Entry& entry, const core::TimePoint now) noexcept {
            entry.stamp(now);
            if (entry.payload > 0) {
                last_data_send_ = now;
            }
        }

        /// RFC 5681's idle test for a send into the empty queue: the last data left more than `rto` ago.
        [[nodiscard]] bool idle(const core::TimePoint now, const core::Duration rto) const noexcept {
            return chain_.empty() && last_data_send_ && now - *last_data_send_ > rto;
        }

        /// Payload bytes the peer acknowledged: what each ACK covered less SYN and FIN.
        [[nodiscard]] std::uint64_t delivered() const noexcept {
            return delivered_;
        }

        /// Every entry back to the pool: the node now, its packet and a copy out now or when the device returns them,
        /// each exactly once; and the node set aside for a FIN that never came, any node's again.
        void drop(Pool& pool) noexcept {
            pool.release_all(chain_);
            if (std::exchange(fin_reserved_, false)) {
                pool.unreserve();
            }
        }

    private:
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
    };

}  // namespace aloe::tcp::detail
