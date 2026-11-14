#pragma once

#include <algorithm>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's receive window: the byte budget, our scale, and the right edge already offered to the
     * peer, as the peer reconstructs it from the scaled field.
     *
     * The edge never moves left. It moves right by full segments only (RFC 1122 4.2.3.3), and a field is rounded up
     * to the scale's unit, so the edge can stand ahead of the budget; the packet cap, not the budget, bounds memory.
     */
    class ReceiveWindow {
    public:
        /// What a segment that carries an ACK states: the window field, and the edge the peer reconstructs from it.
        struct Offer {
            std::uint16_t field = 0;
            wire::TcpSequence edge{};
        };

        static constexpr std::uint32_t unscaled_max = 65535;

        /// Half of what `segments` full segments hold, or `configured` when set, never more than they hold nor more
        /// than the field states under `shift`.
        [[nodiscard]] static constexpr std::uint32_t budget_of(const std::size_t segments,
                                                               const std::uint16_t full_segment,
                                                               const std::uint32_t configured,
                                                               const std::uint8_t shift) noexcept {
            const std::uint64_t held   = std::uint64_t{segments} * full_segment;
            const std::uint64_t wanted = configured != 0 ? std::uint64_t{configured} : held / 2;
            const std::uint64_t field  = std::uint64_t{unscaled_max} << shift;
            return static_cast<std::uint32_t>(std::min({wanted, held, field}));
        }

        /// The smallest shift, at most 14, under which the field states `budget`.
        [[nodiscard]] static constexpr std::uint8_t shift_for(const std::uint32_t budget) noexcept {
            std::uint8_t shift = 0;
            while (shift < max_shift && (std::uint64_t{unscaled_max} << shift) < budget) {
                ++shift;
            }
            return shift;
        }

        /// The handshake settled the options: RCV.NXT is `next`, a full segment carries `full_segment` bytes, and
        /// `scale` is our shift when both ends offered window scaling. The handshake's own segment stated
        /// `min(budget, 65535)`, unscaled, which is where the edge starts.
        void open(const wire::TcpSequence next,
                  const std::uint32_t budget,
                  const std::uint16_t full_segment,
                  const std::optional<std::uint8_t> scale) noexcept {
            budget_    = budget;
            threshold_ = std::max<std::uint32_t>(1, std::min<std::uint32_t>(budget / 2, full_segment));
            scale_     = scale;
            edge_      = next + std::min(budget, unscaled_max);
        }

        [[nodiscard]] std::uint32_t budget() const noexcept {
            return budget_;
        }

        /// What a SYN-ACK's option states.
        [[nodiscard]] std::optional<std::uint8_t> scale() const noexcept {
            return scale_;
        }

        /// `scale()`, or zero.
        [[nodiscard]] std::uint8_t shift() const noexcept {
            return scale_.value_or(std::uint8_t{0});
        }

        [[nodiscard]] wire::TcpSequence edge() const noexcept {
            return edge_;
        }

        /// `edge - next`.
        [[nodiscard]] std::uint32_t window(const wire::TcpSequence next) const noexcept {
            return static_cast<std::uint32_t>(edge_ - next);
        }

        /// What a SYN-ACK states: the field unscaled, `min(budget, 65535)`. A SYN-ACK sent again may follow an ACK
        /// whose scaled field, rounded up, recorded an edge past the field's: the edge is the later of the two, so it
        /// never moves left.
        [[nodiscard]] Offer handshake(const wire::TcpSequence next) const noexcept {
            const std::uint32_t field        = std::min(budget_, unscaled_max);
            const wire::TcpSequence unscaled = next + field;
            return Offer{.field = static_cast<std::uint16_t>(field), .edge = edge_.after(unscaled) ? edge_ : unscaled};
        }

        /// What an ordinary segment with an ACK states now.
        [[nodiscard]] Offer
        offer(const wire::TcpSequence next, const std::size_t unread, const bool slot_free) const noexcept {
            const wire::TcpSequence candidate = candidate_of(next, unread);
            return state(next, slot_free && reaches(candidate) ? candidate : edge_);
        }

        /// What a duplicate ACK states: the edge already offered, whatever was consumed since.
        [[nodiscard]] Offer restate(const wire::TcpSequence next) const noexcept {
            return state(next, edge_);
        }

        /// A segment left stating `offer`; true when that moved the edge.
        [[nodiscard]] bool record(const Offer& offer) noexcept {
            assert(!offer.edge.before(edge_) && "the offered edge never moves left");
            const bool moved = offer.edge != edge_;
            edge_            = offer.edge;
            return moved;
        }

        /// Whether a `consume` owes a window update of its own: Linux's doubling rule (`__tcp_cleanup_rbuf`).
        [[nodiscard]] bool
        update_due(const wire::TcpSequence next, const std::size_t unread, const bool slot_free) const noexcept {
            const wire::TcpSequence candidate = candidate_of(next, unread);
            if (!slot_free || !reaches(candidate)) {
                return false;
            }
            const auto could = static_cast<std::uint64_t>(candidate - next);
            return could >= 2 * std::uint64_t{window(next)};
        }

    private:
        static constexpr std::uint8_t max_shift = 14;  ///< RFC 7323's largest shift, Linux's `TCP_MAX_WSCALE`.

        /// The edge the budget allows now: RCV.NXT plus the budget less the unread bytes.
        [[nodiscard]] wire::TcpSequence candidate_of(const wire::TcpSequence next,
                                                     const std::size_t unread) const noexcept {
            const std::uint32_t room = budget_ > unread ? static_cast<std::uint32_t>(budget_ - unread) : 0U;
            return next + room;
        }

        /// RFC 1122's rule against silly windows: `candidate` lies at least the threshold beyond the edge already
        /// offered.
        [[nodiscard]] bool reaches(const wire::TcpSequence candidate) const noexcept {
            return std::int64_t{candidate - edge_} >= std::int64_t{threshold_};
        }

        /// The field that states `target`, rounded up to the unit, and the edge the peer reconstructs from it.
        [[nodiscard]] Offer state(const wire::TcpSequence next, const wire::TcpSequence target) const noexcept {
            const std::uint8_t bits     = shift();
            const auto distance         = static_cast<std::uint64_t>(target - next);
            const std::uint64_t unit    = std::uint64_t{1} << bits;
            const std::uint64_t rounded = std::min<std::uint64_t>(unscaled_max, (distance + unit - 1) >> bits);
            const auto field            = static_cast<std::uint16_t>(rounded);
            return Offer{.field = field, .edge = next + static_cast<std::uint32_t>(rounded << bits)};
        }

        std::uint32_t budget_    = 0;
        /// RFC 1122's silly-window threshold, fixed at `open`: `min(budget / 2, full segment)`, and at least a byte.
        std::uint32_t threshold_ = 0;
        std::optional<std::uint8_t> scale_{};
        wire::TcpSequence edge_{};
    };

}  // namespace aloe::tcp::detail
