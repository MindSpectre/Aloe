#pragma once

#include <aloe/core>
#include <aloe/wire>
#include <cstdint>
#include <optional>

namespace aloe::tcp::detail {

    /**
     * @brief The tail loss probe of a connection that uses SACK, RFC 8985 section 7: whether a probe is in flight and
     * where it ends, whether its verdict is open, whether a round-trip sample was taken since the last one, the wait,
     * and what an ACK means for it.
     *
     * RFC 8985 keeps one variable, `TLP.end_seq`, for two facts that this class keeps apart. The probe is in flight
     * from the moment it leaves until a cumulative ACK reaches its end, the end of the segment it sent again. That is
     * SND.NXT but while a FIN waits for the peer's window: the stack's SND.NXT counts that FIN from the close, where
     * the RFC's SND.NXT and Linux's `snd_nxt`, which `tlp_high_seq` takes, count only what was sent. Its verdict is
     * open from the moment it leaves until it is known whether it was needless or repaired a loss; an ACK that lands
     * exactly on the probe's end may answer the original or the probe, so the verdict usually outlives the flight. The
     * flight is the RFC's limit of one probe out at a time (7.3); the verdict is what its 7.4 reads.
     *
     * It owns no timer and sends nothing: the Transmitter arms the probe's timer from `wait` and resends the segment,
     * the AckProcessor reads what an ACK settled, and a recovery or a timeout forgets both facts. A probe whose packet
     * the device still holds is the probe from the moment the resend accepts it, to leave at the flush; one whose
     * entry an ACK frees before it leaves never left, and is forgotten too (`Transmitter::acknowledge`). Sequence
     * numbers compare in serial arithmetic, so the probe's end holds across the wrap.
     */
    class TailProbe {
    public:
        /// What an ACK settled.
        enum class Verdict : std::uint8_t {
            Open,      ///< Nothing yet: no ACK at or past the probe's end that says which copy arrived.
            Needless,  ///< Both the original and the probe arrived: nothing was lost.
            Repaired,  ///< An ACK past the probe's end with no duplicate report: the probe repaired a loss.
        };

        /**
         * RFC 8985 7.2: two smoothed round trips, `initial` before a sample, plus `allowance` for one segment.
         *
         * The allowance, `TcpConfig::tail_probe_ack_delay`, is the RFC's `TLP.max_ack_delay`, which it leaves to the
         * implementation ("MAY inflate the PTO"); Linux's `tcp_schedule_loss_probe` adds its minimum RTO, 200 ms, which
         * puts a lone segment's probe at the RTO's moment. Before a sample the allowance still applies, where the RFC's
         * `TLP_calc_PTO` and Linux take the initial wait alone: no probe goes before a sample (`may_probe`), so that
         * wait only says when the timer gives way to the RTO, which it never passes. No floor beyond the timer wheel's
         * tick, which rounds the deadline up: Linux adds 2 ms (`TCP_TIMEOUT_MIN_US`) to a flight of several.
         */
        [[nodiscard]] static core::Duration wait(const std::optional<core::Duration> srtt,
                                                 const bool one_segment,
                                                 const core::Duration allowance,
                                                 const core::Duration initial) noexcept {
            const core::Duration base = srtt ? 2 * *srtt : initial;
            return one_segment ? base + allowance : base;
        }

        /// A probe may leave: none is in flight, and a round-trip sample was taken since the last (RFC 8985 7.3).
        [[nodiscard]] bool may_probe() const noexcept {
            return !in_flight_ && sampled_;
        }

        [[nodiscard]] bool in_flight() const noexcept {
            return in_flight_;
        }

        [[nodiscard]] bool verdict_open() const noexcept {
            return end_.has_value();
        }

        /// The probe's end, while its verdict is open.
        [[nodiscard]] std::optional<wire::TcpSequence> end() const noexcept {
            return end_;
        }

        /// A probe left, or waits to leave at the flush, ending at `end`, the end of the segment it sends again: in
        /// flight, verdict open, no sample since. Its end and its verdict replace any earlier probe's.
        void sent(const wire::TcpSequence end) noexcept {
            end_       = end;
            in_flight_ = true;
            sampled_   = false;
        }

        /// A round-trip sample was taken.
        void sampled() noexcept {
            sampled_ = true;
        }

        /**
         * RFC 8985 7.4's `TLP_process_ack`, Linux's `tcp_process_tlp_ack`. `ack` at or past the end takes the probe out
         * of flight. Then: `duplicate_for_probe` closes the verdict as needless; `ack` past the end closes it as
         * repaired; a `plain_duplicate` (no advance, no data, no block) closes it as needless; otherwise it stays open.
         * Nothing without a verdict open, or for an ACK short of the end.
         */
        [[nodiscard]] Verdict
        on_ack(const wire::TcpSequence ack, const bool duplicate_for_probe, const bool plain_duplicate) noexcept {
            if (!end_ || ack.before(*end_)) {
                return Verdict::Open;
            }
            in_flight_ = false;
            if (duplicate_for_probe) {
                end_.reset();
                return Verdict::Needless;
            }
            if (ack.after(*end_)) {
                end_.reset();
                return Verdict::Repaired;
            }
            if (plain_duplicate) {
                end_.reset();
                return Verdict::Needless;
            }
            return Verdict::Open;
        }

        /// A recovery or a timeout began, or the connection ended: nothing in flight, no verdict.
        void forget() noexcept {
            end_.reset();
            in_flight_ = false;
        }

    private:
        std::optional<wire::TcpSequence> end_{};  ///< The probe's end while its verdict is open; in flight within that.
        bool in_flight_ = false;
        bool sampled_   = false;  ///< A round-trip sample was taken since the last probe left.
    };

}  // namespace aloe::tcp::detail
