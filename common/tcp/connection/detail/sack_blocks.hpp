#pragma once

#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's RFC 2018 selective acknowledgement: whether SACK-permitted was negotiated, the blocks that
     * report what is held above the gap, and the RFC 2883 duplicate the next pure ACK reports first.
     *
     * Negotiated as the timestamps are (`Timestamps`): the SYN or SYN-ACK offers the option when the configuration
     * allows it, and it is in use only when both ends sent it. An active open that ends before the peer answered
     * negotiated nothing.
     */
    class SackBlocks {
    public:
        static constexpr std::size_t capacity = wire::TcpOptions::max_sack_blocks;

        /// What the SYN or SYN-ACK offers.
        void offer(const bool enabled) noexcept {
            in_use_ = enabled;
        }

        /// In use only when both ends sent the option.
        void answer(const bool peer_offered) noexcept {
            in_use_ = in_use_ && peer_offered;
        }

        /// The offer was never answered.
        void withdraw() noexcept {
            in_use_ = false;
        }

        /// In SynSent the SYN's offer; from the handshake on, the option both ends sent.
        [[nodiscard]] bool in_use() const noexcept {
            return in_use_;
        }

        /// [first, end) is held above the gap, or was touched again: the block that contains or touches it is extended
        /// over it, blocks it joins are merged, and the result moves to the front; the oldest falls off past four.
        void held(const wire::TcpSequence first, const wire::TcpSequence end) noexcept {
            if (!in_use_) {
                return;
            }
            wire::TcpSackBlock merged{.first = first, .end = end};
            std::size_t kept = 0;
            for (std::size_t index = 0; index < count_; ++index) {
                const wire::TcpSackBlock block = blocks_[index];
                if (touches(block, merged)) {
                    merged.first = block.first.before(merged.first) ? block.first : merged.first;
                    merged.end   = block.end.after(merged.end) ? block.end : merged.end;
                } else {
                    blocks_[kept++] = block;
                }
            }
            kept = kept < capacity ? kept : capacity - 1;  // past four ranges the oldest falls off
            for (std::size_t index = kept; index > 0; --index) {
                blocks_[index] = blocks_[index - 1];
            }
            blocks_[0] = merged;
            count_     = static_cast<std::uint8_t>(kept + 1);
        }

        /// RCV.NXT moved to `next`: blocks at or below it go, and one it passed in part starts at it.
        void advanced(const wire::TcpSequence next) noexcept {
            std::size_t kept = 0;
            for (std::size_t index = 0; index < count_; ++index) {
                wire::TcpSackBlock block = blocks_[index];
                if (!next.before(block.end)) {
                    continue;
                }
                if (next.after(block.first)) {
                    block.first = next;
                }
                blocks_[kept++] = block;
            }
            count_ = static_cast<std::uint8_t>(kept);
        }

        /// Held data [first, end), the tail of its block, was released: the block ends at `first`, or goes. A block
        /// that starts inside the range keeps what follows it; one that holds the range in its middle ends at `first`,
        /// which under-reports what is still held but never reports what is not.
        void released(const wire::TcpSequence first, const wire::TcpSequence end) noexcept {
            std::size_t kept = 0;
            for (std::size_t index = 0; index < count_; ++index) {
                wire::TcpSackBlock block = blocks_[index];
                if (first.before(block.end) && block.first.before(end)) {
                    if (block.first.before(first)) {
                        block.end = first;
                    } else if (end.before(block.end)) {
                        block.first = end;
                    } else {
                        continue;
                    }
                }
                blocks_[kept++] = block;
            }
            count_ = static_cast<std::uint8_t>(kept);
        }

        /// [first, end) arrived twice. One duplicate is pending: a second extends it when they touch, else is dropped.
        void duplicate(const wire::TcpSequence first, const wire::TcpSequence end) noexcept {
            if (!in_use_) {
                return;
            }
            const wire::TcpSackBlock range{.first = first, .end = end};
            if (!duplicate_) {
                duplicate_ = range;
            } else if (touches(*duplicate_, range)) {
                duplicate_->first = first.before(duplicate_->first) ? first : duplicate_->first;
                duplicate_->end   = end.after(duplicate_->end) ? end : duplicate_->end;
            }
        }

        /// A segment with payload arrived: a duplicate not yet reported is forgotten (Linux, `tcp_data_queue`).
        void forget_duplicate() noexcept {
            duplicate_.reset();
        }

        /// The next pure ACK carries the option: SACK is in use and a block is held or a duplicate pending.
        [[nodiscard]] bool pending() const noexcept {
            return in_use_ && (count_ > 0 || duplicate_.has_value());
        }

        /// What the next pure ACK carries, at most `room` blocks: the duplicate first, then the blocks in order.
        [[nodiscard]] std::size_t report(const std::span<wire::TcpSackBlock, capacity> out,
                                         const std::size_t room) const noexcept {
            std::size_t written = 0;
            if (duplicate_ && written < room) {
                out[written++] = *duplicate_;
            }
            for (std::size_t index = 0; index < count_ && written < room; ++index) {
                out[written++] = blocks_[index];
            }
            return written;
        }

        [[nodiscard]] bool duplicate_pending() const noexcept {
            return duplicate_.has_value();
        }

        /// An ACK that carried the report left: the duplicate is forgotten.
        void reported() noexcept {
            duplicate_.reset();
        }

        /// The blocks and the duplicate go; the negotiation stays. At the peer's FIN and at a terminal event.
        void drop() noexcept {
            count_ = 0;
            duplicate_.reset();
        }

        /// The blocks held, the duplicate not counted.
        [[nodiscard]] std::size_t count() const noexcept {
            return count_;
        }

    private:
        /// They overlap or touch, Linux's `tcp_sack_extend`.
        [[nodiscard]] static bool touches(const wire::TcpSackBlock& block, const wire::TcpSackBlock& range) noexcept {
            return !range.first.after(block.end) && !block.first.after(range.end);
        }

        std::array<wire::TcpSackBlock, capacity> blocks_{};  ///< In report order: the most recently changed first.
        std::optional<wire::TcpSackBlock> duplicate_{};      ///< RFC 2883: the range the next pure ACK reports first.
        std::uint8_t count_ = 0;
        bool in_use_        = false;
    };

}  // namespace aloe::tcp::detail
