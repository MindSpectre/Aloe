#pragma once

#include <algorithm>
#include <aloe/wire>
#include <cassert>
#include <cstdint>
#include <limits>

#include <congestion_control.hpp>
#include <detail/rate_reduction.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief A connection's loss recovery: one machine of three states over the congestion control's per-connection
     * state, for every connection, with SACK and without.
     *
     * Open, nothing is known lost. Recovery runs from the first entry marked lost to the ACK that covers `recover`,
     * the last sequence sent by then; the policy sets its target as it begins, and RFC 9937's rate reduction regulates
     * the window down to it (`RateReduction`), unless the policy's target is unbounded, in which case its window
     * stands. Loss is the episode after an expiry of the retransmission timer, to the ACK that covers `recover`: the
     * policy's window restarts and grows by its own rules.
     *
     * `recover` is RFC 6582's: the highest sequence outstanding when the last episode began. An episode ends at the
     * ACK that covers it, and a new recovery is admitted only by an ACK that covers more (`admits`), so duplicates the
     * resends of a timeout can explain begin none. While open, every ACK of new data slides `recover` to just below
     * the last byte acknowledged, which that ACK then covers more than: an ordinary loss is always admitted, and the
     * serial comparison never sees `recover` half the space behind.
     *
     * It decides nothing about which entries are lost or resent: the stack marks, tells it what happened, and reads
     * the window. It owns the state, `recover`, the duplicate count, the policy's state and the copy of it taken as an
     * episode begins, for the undo, and it holds the rate reduction.
     *
     * The duplicate count also stands in for the peer's reports on a connection without SACK, as Linux emulates them
     * (`tcp_add_reno_sack`, `tcp_remove_reno_sacks`), in bytes: each duplicate is one segment the peer holds above the
     * hole (`duplicate_bytes`), and an ACK that moves SND.UNA in recovery takes back the segments it covers but for
     * the hole it filled (`remove_duplicates`).
     */
    template <typename CongestionControl>
    class LossRecovery {
    public:
        using State = CongestionControl::State;

        /// The connection numbered its SYN: `recover` starts one below the ISS, nothing having been outstanding before
        /// it, so the ACK of the SYN covers more and the loss of the first data segment is admitted. RFC 6582 starts
        /// it at the ISS, which under its own test would refuse that loss.
        void open(const wire::TcpSequence iss) noexcept {
            recover_ = wire::TcpSequence{iss.value - 1};
        }

        /// Synchronized: the policy's initial window.
        void establish(const std::uint16_t mss) noexcept {
            CongestionControl::reset(congestion_, mss);
        }

        /// A send after an idle longer than the RTO: RFC 5681's restart.
        void on_idle(const std::uint16_t mss) noexcept {
            CongestionControl::on_idle(congestion_, mss);
        }

        [[nodiscard]] RecoveryState state() const noexcept {
            return state_;
        }

        [[nodiscard]] bool in_recovery() const noexcept {
            return state_ == RecoveryState::Recovery;
        }

        /// The policy's window; in recovery, PRR's when the policy's target is bounded.
        [[nodiscard]] std::uint32_t window() const noexcept {
            return regulated() ? rate_.window() : CongestionControl::window(congestion_);
        }

        /// What the congestion window allows beyond `in_flight`, zero when `in_flight` fills it; in 64 bits.
        [[nodiscard]] std::uint64_t room(const std::uint32_t in_flight) const noexcept {
            const std::uint64_t limit = window();
            return limit > in_flight ? limit - in_flight : 0;
        }

        /// RFC 9937: nothing is sent once what is in flight matches or exceeds the window.
        [[nodiscard]] bool may_send(const std::uint32_t in_flight) const noexcept {
            return in_flight < window();
        }

        /// A duplicate ACK: counted, saturating. Returns the count.
        [[nodiscard]] std::uint8_t count_duplicate() noexcept {
            if (dupacks_ < std::numeric_limits<std::uint8_t>::max()) {
                ++dupacks_;
            }
            return dupacks_;
        }

        /// Duplicate ACKs counted, saturating. An ACK of new data clears the count outside recovery; in recovery it
        /// stays, less, on a connection without SACK, what such an ACK takes back (`remove_duplicates`).
        [[nodiscard]] std::uint8_t duplicates() const noexcept {
            return dupacks_;
        }

        /// Without SACK, what the duplicates stand for as received above the hole: one segment each, Linux's
        /// `tcp_add_reno_sack` in bytes, before the caller's cap. Nothing in the loss state, where Linux counts a
        /// duplicate only for the new data F-RTO sent (`tcp_process_loss`), and F-RTO is not built: after a timeout a
        /// duplicate says nothing of what the peer holds.
        [[nodiscard]] std::uint32_t duplicate_bytes(const std::uint16_t mss) const noexcept {
            return state_ == RecoveryState::Loss ? 0 : std::uint32_t{dupacks_} * mss;
        }

        /**
         * Without SACK, in recovery: an ACK moved SND.UNA by `advance`. Linux's `tcp_remove_reno_sacks` in bytes. The
         * duplicates had counted the segments above the hole as received, so the ACK delivered the advance less one
         * segment per duplicate counted, and at least one segment, the hole it filled; and the count gives up the
         * segments the advance covers, a part of one as one, less that one. Returns what the ACK delivered.
         *
         * A departure from RFC 9937 6.2, which is definite here: without SACK its DeliveredData on an advance is
         * "the change in SND.UNA, minus 1 SMSS for each preceding duplicate ACK", with no floor, and its duplicates
         * are counted over the whole episode. Linux's emulation is followed instead (`tcp_add_reno_sack`,
         * `tcp_remove_reno_sacks`, `tcp_limit_reno_sacked`). The RFC's count, never reduced, subtracts again after a
         * partial ACK what the advance already took out of the flight, so it under-reads what is in flight, and what
         * the next advance delivered, by the segments between the holes; Linux's removal on an advance is exact for
         * those, and its floor of one segment is the hole the ACK filled, which no duplicate had counted.
         */
        [[nodiscard]] std::uint32_t remove_duplicates(const std::uint32_t advance, const std::uint16_t mss) noexcept {
            const std::uint32_t segment  = std::max<std::uint16_t>(mss, 1);
            const std::uint64_t counted  = std::uint64_t{dupacks_} * segment;
            const std::uint32_t segments = advance / segment + (advance % segment != 0 ? 1U : 0U);
            if (segments > 1) {
                dupacks_ = static_cast<std::uint8_t>(dupacks_ - std::min<std::uint32_t>(dupacks_, segments - 1));
            }
            return advance > counted + segment ? static_cast<std::uint32_t>(advance - counted) : segment;
        }

        /// In recovery: what the episode may still count as delivered before its total passes RecoverFS. RFC 9937 6.2
        /// holds a connection without SACK to it, since its duplicates are an estimate a peer can inflate.
        [[nodiscard]] std::uint32_t undelivered() const noexcept {
            return rate_.recover_fs() > rate_.delivered() ? rate_.recover_fs() - rate_.delivered() : 0;
        }

        /// RFC 6582 3.2: the third duplicate starts recovery only when `ack` covers more than `recover`, which it
        /// defines as `ack_number - 1 > recover`: at least one byte above the highest sequence outstanding when the
        /// last episode began is acknowledged. The guard against a second fast retransmit for data a timeout already
        /// resent. Linux reaches the same end by holding its recovery state until something above its mark is
        /// acknowledged (`tcp_is_non_sack_preventing_reopen`).
        [[nodiscard]] bool admits(const wire::TcpSequence ack) const noexcept {
            return wire::TcpSequence{ack.value - 1}.after(recover_);
        }

        /**
         * The first entry was marked lost while open: `recover` is one before `nxt`, the policy's state is copied for
         * the undo, the policy is told of the congestion, and PRR begins with `recover_fs`.
         *
         * The policy hears `flight`, SND.NXT - SND.UNA, less what was sent beyond its window. RFC 5681 3.2 step 2: data
         * sent by limited transmit "MUST NOT be included" in the flight the target is computed from. Limited transmit
         * has no allowance of its own here: it is what went out beyond the policy's window because reports or
         * duplicates lowered what is in flight, and a send never passes the peer's window, so the cap is the policy's
         * window as it stands. RecoverFS is not capped: RFC 9937 6.1 counts the whole flight.
         */
        void begin_recovery(const wire::TcpSequence nxt,
                            const std::uint32_t flight,
                            const std::uint32_t recover_fs,
                            const std::uint16_t mss) noexcept {
            assert(state_ != RecoveryState::Recovery && "an episode of recovery begins once");
            recover_ = wire::TcpSequence{nxt.value - 1};
            prior_   = congestion_;
            CongestionControl::on_congestion(
                congestion_, std::min(flight, CongestionControl::window(congestion_)), mss);
            state_ = RecoveryState::Recovery;
            if (regulated()) {
                rate_.begin(recover_fs, CongestionControl::target(congestion_));
            }
        }

        /// One ACK in recovery that does not end it.
        void on_recovery_ack(const std::uint32_t delivered,
                             const std::uint32_t in_flight,
                             const bool safe,
                             const std::uint16_t mss) noexcept {
            assert(state_ == RecoveryState::Recovery);
            if (regulated()) {
                rate_.on_ack(delivered, in_flight, safe, mss);
            }
        }

        /// Recovery began or went on at a timer, with no ACK.
        void on_recovery_timer(const std::uint32_t in_flight, const std::uint16_t mss) noexcept {
            assert(state_ == RecoveryState::Recovery);
            if (regulated()) {
                rate_.allow_one(in_flight, mss);
            }
        }

        /// Sequence space left while in recovery, new or resent. Outside recovery nothing counts it.
        void on_sent(const std::uint32_t bytes) noexcept {
            if (regulated()) {
                rate_.on_sent(bytes);
            }
        }

        /// `ack` covers `recover`: the episode, recovery or loss, is over.
        [[nodiscard]] bool recovered(const wire::TcpSequence ack) const noexcept {
            return ack.after(recover_);
        }

        /// The episode ends: from recovery the policy's window becomes its target; the state is open and the duplicate
        /// count is cleared. `recover` slides with `ack` only when `ack` covers more than it. An episode that ends at
        /// exactly the ACK that covers `recover` leaves it in place, so duplicates at that acknowledgement begin no
        /// recovery until an ACK of new data moves SND.UNA above it.
        void end(const wire::TcpSequence ack) noexcept {
            if (state_ == RecoveryState::Recovery) {
                CongestionControl::on_recovery_end(congestion_);
            }
            state_   = RecoveryState::Open;
            dupacks_ = 0;
            if (admits(ack)) {
                slide(ack);
            }
        }

        /**
         * The RTO expired. The policy's target is set again only when this window was not reduced yet: the state was
         * open, or `una` already covers `recover`, or the state was loss and `first_expiry` (the first since an ACK of
         * new data); Linux's `tcp_enter_loss`. Then the policy's state is copied first, as when recovery begins, and
         * the policy hears the flight as it is. In every case the policy's window restarts at `in_flight` plus one
         * segment, the state is loss, `recover` is one before `nxt` and the duplicate count is cleared.
         */
        void on_timeout(const wire::TcpSequence una,
                        const wire::TcpSequence nxt,
                        const std::uint32_t flight,
                        const std::uint32_t in_flight,
                        const std::uint16_t mss,
                        const bool first_expiry) noexcept {
            if (state_ == RecoveryState::Open || una.after(recover_) ||
                (state_ == RecoveryState::Loss && first_expiry)) {
                prior_ = congestion_;
                CongestionControl::on_congestion(congestion_, flight, mss);
            }
            CongestionControl::on_timeout(congestion_, in_flight, mss);
            state_   = RecoveryState::Loss;
            recover_ = wire::TcpSequence{nxt.value - 1};
            dupacks_ = 0;
        }

        /**
         * An ACK of new data, open or in the loss state: the policy hears of it; open, `recover` slides with `ack`.
         * The duplicate count is cleared. The guard compares in serial arithmetic, which reads a `recover` left more
         * than 2^31 behind as ahead, so one set at the open or by the last episode would refuse every recovery after
         * 2 GiB; hence the slide. In the loss state `recover` stays: the episode is measured against it.
         */
        void on_ack(const AckInfo& info, const wire::TcpSequence ack) noexcept {
            assert(state_ != RecoveryState::Recovery && "the window in recovery is not the policy's to grow");
            dupacks_ = 0;
            CongestionControl::on_ack(congestion_, info);
            if (state_ == RecoveryState::Open) {
                slide(ack);
            }
        }

        [[nodiscard]] const State& congestion() const noexcept {
            return congestion_;
        }

        /// For tests: `recover` takes this value, as if an episode had recorded it.
        void force_recover(const wire::TcpSequence recover) noexcept {
            recover_ = recover;
        }

    private:
        /// `recover` to just below the last byte `ack` acknowledges, so that `ack` covers more than it (`admits`).
        void slide(const wire::TcpSequence ack) noexcept {
            recover_ = wire::TcpSequence{ack.value - 2};
        }

        /// PRR runs, and its window is the connection's: in recovery, under a policy whose target is bounded. A policy
        /// with no target has nothing to reduce, as Linux leaves the window alone when the threshold is infinite
        /// (`tcp_end_cwnd_reduction`).
        [[nodiscard]] bool regulated() const noexcept {
            return state_ == RecoveryState::Recovery &&
                   CongestionControl::target(congestion_) < std::numeric_limits<std::uint32_t>::max();
        }

        RecoveryState state_  = RecoveryState::Open;
        std::uint8_t dupacks_ = 0;
        wire::TcpSequence recover_{};  ///< The highest sequence out as the episode began; open, slid below SND.UNA.
        State congestion_{};           ///< The policy's state; reset where the connection is Established.
        State prior_{};                ///< The policy's state as the episode began: what an undo restores.
        RateReduction rate_{};         ///< The window in recovery.
    };

}  // namespace aloe::tcp::detail
