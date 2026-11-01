#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <held_segment_pool.hpp>
#include <node_pool.hpp>
#include <packet.hpp>
#include <read_view.hpp>
#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's receive side: RCV.NXT, the right edge already offered to the peer, and the payload it holds
     * for the application, chained on the shard's pool of held segments.
     *
     * The pool is the stack's and is passed in; the queue holds the chain. The offered edge never retreats. Once the
     * peer's FIN is in, `closed()` and nothing more is accepted.
     */
    template <device::IsPacket Packet>
    class ReceiveQueue {
    public:
        using Pool = HeldSegmentPool<Packet>;

        /// The peer's ISN is in: RCV.NXT follows it, and the first offer, carried by the handshake, is `budget` bytes.
        void open(const wire::TcpSequence next, const std::uint32_t budget) noexcept {
            next_    = next;
            offered_ = next + budget;
        }

        /// RCV.NXT.
        [[nodiscard]] wire::TcpSequence next() const noexcept {
            return next_;
        }

        /// The right edge already offered.
        [[nodiscard]] wire::TcpSequence offered() const noexcept {
            return offered_;
        }

        /// The window offered: from RCV.NXT to the offered edge.
        [[nodiscard]] std::uint32_t window() const noexcept {
            return offered_.value - next_.value;
        }

        [[nodiscard]] std::size_t unread() const noexcept {
            return unread_;
        }

        /// Held packets.
        [[nodiscard]] std::uint32_t segments() const noexcept {
            return chain_.count;
        }

        /// The peer's FIN is in: its sequence space ended.
        [[nodiscard]] bool closed() const noexcept {
            return closed_;
        }

        /// The unconsumed payloads, in order; invalidated by `consume`, `hold` and `release`.
        [[nodiscard]] ReadView<Packet> view(Pool& pool) const noexcept {
            return ReadView<Packet>{pool, chain_.head, unread_};
        }

        /**
         * Holds `length` payload bytes at `offset` of `packet`, in order: RCV.NXT moves past them. False, with the
         * packet still the caller's, when the pool has no node free.
         */
        [[nodiscard]] bool
        hold(Pool& pool, Packet& packet, const std::uint16_t offset, const std::uint16_t length) noexcept {
            const std::optional<std::uint32_t> index = pool.acquire();
            if (!index) {
                return false;
            }
            HeldSegment<Packet>& node = pool.node(*index);
            node.packet               = std::move(packet);
            node.offset               = offset;
            node.length               = length;
            pool.push_back(chain_, *index);
            next_    = next_ + length;
            unread_ += length;
            return true;
        }

        /// The peer's FIN, in order: it takes one number, and the peer's sequence space ends with it.
        void take_fin() noexcept {
            next_   = next_ + 1;
            closed_ = true;
        }

        /// Advances the front by `count` bytes, at most `unread()`, freeing packets as they empty.
        void consume(Pool& pool, const std::size_t count) noexcept {
            assert(count <= unread_ && "consume past unread()");
            std::size_t remaining = count;
            while (remaining > 0) {
                HeldSegment<Packet>& node  = pool.node(chain_.head);
                const std::size_t taken    = std::min<std::size_t>(remaining, node.length);
                node.offset                = static_cast<std::uint16_t>(node.offset + taken);
                node.length                = static_cast<std::uint16_t>(node.length - taken);
                remaining                 -= taken;
                unread_                   -= taken;
                if (node.length == 0) {
                    pool.release_front(chain_);
                }
            }
        }

        /**
         * The right edge an ACK offers now: `next + (budget - unread)` when it is later than the edge already offered
         * and fewer than `cap` packets are held, else the edge already offered.
         */
        [[nodiscard]] wire::TcpSequence offer(const std::uint32_t budget, const std::size_t cap) const noexcept {
            if (const wire::TcpSequence candidate = next_ + (budget - static_cast<std::uint32_t>(unread_));
                candidate.after(offered_) && chain_.count < cap) {
                return candidate;
            }
            return offered_;
        }

        /// A segment that left offered `edge`; true when that moved the edge.
        [[nodiscard]] bool record_offer(const wire::TcpSequence edge) noexcept {
            return std::exchange(offered_, edge) != edge;
        }

        /// Every held packet back to the pool.
        void release(Pool& pool) noexcept {
            pool.release_all(chain_);
            unread_ = 0;
        }

    private:
        wire::TcpSequence next_{};     ///< RCV.NXT.
        wire::TcpSequence offered_{};  ///< The right edge already offered to the peer; never retreats.
        std::size_t unread_ = 0;
        NodeChain chain_{};
        bool closed_ = false;
    };

}  // namespace aloe::tcp::detail
