#pragma once

#include <aloe/device>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <detail/held_segment_pool.hpp>
#include <detail/node_pool.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's out-of-order payload: a chain of the shard's held-segment nodes sorted by sequence, each
     * the packet a segment arrived in, and the sequence of a FIN that arrived above a gap.
     *
     * The ranges are disjoint and none reaches RCV.NXT: what reaches it is taken by `take_run`. Nothing is copied:
     * an overlap moves a node's offset, length and first sequence.
     */
    template <device::IsPacket Packet>
    class ReassemblyQueue {
    public:
        using Pool = HeldSegmentPool<Packet>;

        enum class Placed : std::uint8_t {
            Held,       ///< The packet was moved into a node; `kept` is the range it holds after the trims.
            Duplicate,  ///< Held data covers every byte: nothing is kept and the packet is still the caller's.
            NoNode,     ///< The pool had no node: nothing is kept and the packet is still the caller's.
        };

        struct Inserted {
            Placed placed = Placed::Held;
            wire::TcpSackBlock kept{};
            std::optional<wire::TcpSackBlock> duplicate{};  ///< The first range of the segment that was already held.
        };

        /// What the join took: nodes still taken, linked in order, ready for `ReceiveQueue::append`.
        struct Run {
            NodeChain chain{};
            std::uint32_t bytes = 0;
            std::optional<wire::TcpSackBlock> duplicate{};  ///< Held data that RCV.NXT had already passed.
        };

        /// A node is held or a FIN is remembered: the one question an in-order segment asks.
        [[nodiscard]] bool holds() const noexcept {
            return !chain_.empty() || fin_.has_value();
        }

        [[nodiscard]] std::uint32_t segments() const noexcept {
            return chain_.count;
        }

        [[nodiscard]] std::size_t bytes() const noexcept {
            return bytes_;
        }

        /// The range of the node with the highest sequence.
        [[nodiscard]] std::optional<wire::TcpSackBlock> highest() const noexcept {
            if (chain_.empty()) {
                return std::nullopt;
            }
            return highest_;
        }

        [[nodiscard]] std::optional<wire::TcpSequence> fin() const noexcept {
            return fin_;
        }

        /// Places `length` bytes at `offset` of `packet`, the first of which is `first`, by the three rules.
        [[nodiscard]] Inserted insert(Pool& pool,
                                      Packet& packet,
                                      const std::uint16_t offset,
                                      const std::uint16_t length,
                                      const wire::TcpSequence first) noexcept {
            assert(length > 0 && "an out-of-order segment carries payload");
            wire::TcpSequence start = first;
            wire::TcpSequence end   = first + length;
            if (chain_.empty() || !start.before(range_of(pool.node(chain_.tail)).end)) {
                return place(pool, packet, offset, first, start, end, chain_.tail, std::nullopt);
            }

            std::optional<wire::TcpSackBlock> duplicate;
            std::uint32_t previous = no_node;
            std::uint32_t current  = chain_.head;
            while (current != no_node && !start.before(range_of(pool.node(current)).end)) {
                previous = current;
                current  = pool.node(current).next;
            }
            if (current != no_node) {
                const wire::TcpSackBlock held = range_of(pool.node(current));
                if (!held.first.after(start)) {
                    if (!held.end.before(end)) {
                        duplicate = wire::TcpSackBlock{start, end};
                        return Inserted{.placed = Placed::Duplicate, .kept = {}, .duplicate = duplicate};
                    }
                    if (held.first.before(start)) {
                        duplicate = wire::TcpSackBlock{start, held.end};
                        start     = held.end;
                        previous  = current;
                        current   = pool.node(current).next;
                    }
                }
            }
            while (current != no_node && pool.node(current).first.before(end)) {
                const wire::TcpSackBlock held = range_of(pool.node(current));
                if (held.end.after(end)) {
                    if (!duplicate) {
                        duplicate = wire::TcpSackBlock{held.first, end};
                    }
                    end = held.first;
                    break;
                }
                if (!duplicate) {
                    duplicate = held;
                }
                current = pool.node(current).next;
                drop(pool, pool.unlink_after(chain_, previous));
            }
            if (start == end) {
                return Inserted{.placed = Placed::Duplicate, .kept = {}, .duplicate = duplicate};
            }
            return place(pool, packet, offset, first, start, end, previous, duplicate);
        }

        /// Unlinks the run that follows `next`, RCV.NXT after an in-order segment was held.
        [[nodiscard]] Run take_run(Pool& pool, const wire::TcpSequence next) noexcept {
            Run run;
            wire::TcpSequence at = next;
            while (!chain_.empty()) {
                HeldSegment<Packet>& head     = pool.node(chain_.head);
                const wire::TcpSackBlock held = range_of(head);
                if (held.first.after(at)) {
                    break;
                }
                if (!held.end.after(at)) {
                    report(run.duplicate, held);
                    drop(pool, pool.unlink_after(chain_, no_node));
                    continue;
                }
                if (held.first.before(at)) {
                    const auto passed = static_cast<std::uint16_t>(at - held.first);
                    report(run.duplicate, wire::TcpSackBlock{held.first, at});
                    head.offset  = static_cast<std::uint16_t>(head.offset + passed);
                    head.length  = static_cast<std::uint16_t>(head.length - passed);
                    head.first   = at;
                    bytes_      -= passed;
                }
                run.bytes += head.length;
                bytes_    -= head.length;
                at         = at + head.length;
                pool.push_back(run.chain, pool.unlink_after(chain_, no_node));
            }
            return run;
        }

        /// Releases the node with the highest sequence and forgets a remembered FIN; the range it held.
        [[nodiscard]] wire::TcpSackBlock release_highest(Pool& pool) noexcept {
            assert(!chain_.empty() && "a release from an empty queue");
            std::uint32_t previous = no_node;
            for (std::uint32_t index = chain_.head; index != chain_.tail; index = pool.node(index).next) {
                previous = index;
            }
            const wire::TcpSackBlock released  = highest_;
            bytes_                            -= pool.node(chain_.tail).length;
            pool.release_back(chain_, previous);
            if (previous != no_node) {
                highest_ = range_of(pool.node(previous));
            }
            fin_.reset();
            return released;
        }

        void remember_fin(const wire::TcpSequence at) noexcept {
            fin_ = at;
        }

        void forget_fin() noexcept {
            fin_.reset();
        }

        /// Every node back to the pool, and the FIN forgotten.
        void release(Pool& pool) noexcept {
            pool.release_all(chain_);
            bytes_ = 0;
            fin_.reset();
        }

    private:
        [[nodiscard]] static wire::TcpSackBlock range_of(const HeldSegment<Packet>& node) noexcept {
            return wire::TcpSackBlock{node.first, node.first + node.length};
        }

        /// Records `block` as the run's duplicate when none is yet, or extends the one recorded when the two touch.
        static void report(std::optional<wire::TcpSackBlock>& duplicate, const wire::TcpSackBlock block) noexcept {
            if (!duplicate) {
                duplicate = block;
            } else if (duplicate->end == block.first) {
                duplicate->end = block.end;
            }
        }

        /// Releases an unlinked node of this queue.
        void drop(Pool& pool, const std::uint32_t index) noexcept {
            bytes_ -= pool.node(index).length;
            pool.release(index);
        }

        /// Moves `packet` into a fresh node holding [start, end), whose payload began at `offset` with `first`, and
        /// links it after `previous`.
        [[nodiscard]] Inserted place(Pool& pool,
                                     Packet& packet,
                                     const std::uint16_t offset,
                                     const wire::TcpSequence first,
                                     const wire::TcpSequence start,
                                     const wire::TcpSequence end,
                                     const std::uint32_t previous,
                                     const std::optional<wire::TcpSackBlock> duplicate) noexcept {
            const std::optional<std::uint32_t> index = pool.acquire();
            if (!index) {
                return Inserted{.placed = Placed::NoNode, .kept = {}, .duplicate = std::nullopt};
            }
            HeldSegment<Packet>& node = pool.node(*index);
            node.packet               = std::move(packet);
            node.offset               = static_cast<std::uint16_t>(offset + (start - first));
            node.length               = static_cast<std::uint16_t>(end - start);
            node.first                = start;
            pool.insert_after(chain_, previous, *index);
            bytes_ += node.length;
            if (chain_.tail == *index) {
                highest_ = range_of(node);
            }
            return Inserted{.placed = Placed::Held, .kept = range_of(node), .duplicate = duplicate};
        }

        NodeChain chain_{};
        std::size_t bytes_ = 0;
        wire::TcpSackBlock highest_{};  ///< The tail's range, while the chain is not empty.
        std::optional<wire::TcpSequence> fin_{};
    };

}  // namespace aloe::tcp::detail
