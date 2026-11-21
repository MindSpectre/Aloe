#pragma once

#include <algorithm>
#include <aloe/core>
#include <aloe/wire>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

#include <congestion_control.hpp>
#include <detail/rate_reduction.hpp>
#include <tcp_config.hpp>

namespace aloe::tcp::detail {

    /// What resends of earlier episodes leave in the retransmission queue as an episode begins: where the undo's record
    /// starts (`LossRecovery::begin_recovery`, `LossRecovery::on_timeout`). The queue's, through the caller.
    struct EarlierResends {
        /// Resends a lost mark owed that are out, not marked lost again since, neither reported nor acknowledged:
        /// Linux's `retrans_out`. Each may yet be reported as a duplicate.
        std::uint32_t out   = 0;
        /// Some entry neither reported nor acknowledged was resent by an episode, out or marked lost again since:
        /// Linux's `tcp_any_retrans_done`. An ACK may yet answer that resend. A tail probe's resend alone is none.
        bool unacknowledged = false;
    };

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
     * the window. It owns the state, `recover`, the duplicate count, the policy's state and the undo's record, and it
     * holds the rate reduction.
     *
     * The undo's record (Linux's `tcp_init_undo`) is opened by every episode that sets a target: the episode's start,
     * SND.UNA as it began; the policy's state then; the stamp of the first resend, which an earlier episode's resend
     * still unacknowledged keeps; and how many resends may still be reported as duplicates, which starts at the earlier
     * resends still out, and one more for a tail probe still in flight. A new episode replaces the record and an undo
     * closes it, so an episode is undone at most once; the end of an episode leaves it open, since the duplicate
     * reports trail the resends. Its two signs (`tcp_may_undo`): every resend counted was reported as a duplicate
     * (`every_resend_reported`), or an ACK that moved SND.UNA answers an original (`answers_original`). Which sign is
     * read on which ACK, and what else an undo clears, is the stack's.
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
        /// `tcp_add_reno_sack` in bytes, before the caller's cap. Nothing in the loss state, which is stricter than
        /// Linux: `tcp_process_loss` counts a duplicate there whenever new data went out after the timeout
        /// (`after(snd_nxt, high_seq)`), which its F-RTO sends. F-RTO is not built, and here a duplicate after a
        /// timeout stands for nothing the peer holds.
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
         * An entry was marked lost while open, or a resend was found lost again in the loss state: `recover` is one
         * before `nxt`, the undo's record opens with the policy's state copied, `nxt - flight`, SND.UNA, as the
         * episode's start and `earlier` as what earlier resends leave (`open_undo`), the policy is told of the
         * congestion, which from the loss state too sets a new target, as Linux's `tcp_enter_recovery` does, and PRR
         * begins with `recover_fs`.
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
                            const std::uint16_t mss,
                            const EarlierResends earlier = {}) noexcept {
            assert(state_ != RecoveryState::Recovery && "an episode of recovery begins once");
            recover_ = wire::TcpSequence{nxt.value - 1};
            open_undo(wire::TcpSequence{nxt.value - flight}, earlier);
            CongestionControl::on_congestion(
                congestion_, std::min(flight, CongestionControl::window(congestion_)), mss);
            state_ = RecoveryState::Recovery;
            if (regulated()) {
                rate_.begin(recover_fs, CongestionControl::target(congestion_));
            }
        }

        /**
         * A tail probe repaired a loss (RFC 8985 7.4.2): the third way the policy hears of congestion. It is told once,
         * with `flight` capped by its window as `begin_recovery` tells it, sets its target, and its window becomes that
         * target at once: `on_congestion` then `on_recovery_end`, the cut a fast recovery makes, with nothing resent,
         * no state entered and no undo opened. Linux's `tcp_process_tlp_ack` makes the same cut through
         * `tcp_init_cwnd_reduction` and `tcp_end_cwnd_reduction`. Only while open: a recovery or a timeout forgets the
         * probe whose verdict this settles.
         */
        void on_repaired(const std::uint32_t flight, const std::uint16_t mss) noexcept {
            assert(state_ == RecoveryState::Open && "a probe's verdict is forgotten when an episode begins");
            CongestionControl::on_congestion(
                congestion_, std::min(flight, CongestionControl::window(congestion_)), mss);
            CongestionControl::on_recovery_end(congestion_);
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

        /// The episode ends: from recovery the policy's window becomes its target, unless the episode was undone, as
        /// Linux's `tcp_end_cwnd_reduction` tests `undo_marker`; the state is open and the duplicate count is cleared.
        /// The undo's record stays open. `recover` slides with `ack` only when `ack` covers more than it. An episode
        /// that ends at exactly the ACK that covers `recover` leaves it in place, so duplicates at that acknowledgement
        /// begin no recovery until an ACK of new data moves SND.UNA above it.
        void end(const wire::TcpSequence ack) noexcept {
            if (state_ == RecoveryState::Recovery && undo_open_) {
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
         * new data); Linux's `tcp_enter_loss`. Then the undo's record opens first, with `una` as the episode's start,
         * the policy's state copied and `earlier` as what earlier resends leave, after the timeout's marks, as when
         * recovery begins (`open_undo`), and the policy hears the flight as it is; otherwise the record stays the one
         * this window's reduction opened. In every case the policy's window restarts at `in_flight` plus one segment,
         * the state is loss, `recover` is one before `nxt` and the duplicate count is cleared.
         */
        void on_timeout(const wire::TcpSequence una,
                        const wire::TcpSequence nxt,
                        const std::uint32_t flight,
                        const std::uint32_t in_flight,
                        const std::uint16_t mss,
                        const bool first_expiry,
                        const EarlierResends earlier = {}) noexcept {
            if (state_ == RecoveryState::Open || una.after(recover_) ||
                (state_ == RecoveryState::Loss && first_expiry)) {
                open_undo(una, earlier);
                CongestionControl::on_congestion(congestion_, flight, mss);
            }
            CongestionControl::on_timeout(congestion_, in_flight, mss);
            state_   = RecoveryState::Loss;
            recover_ = wire::TcpSequence{nxt.value - 1};
            dupacks_ = 0;
        }

        /**
         * An ACK of new data, open or in the loss state: the policy hears of it; open, `recover` slides with `ack`,
         * but for an episode's `recover` that `ack` does not reach, which only an undo that reopened short of it leaves
         * (`holds_guard`). The duplicate count is cleared. The guard compares in serial arithmetic, which reads a
         * `recover` left more than 2^31 behind as ahead, so one set at the open or by the last episode would refuse
         * every recovery after 2 GiB; hence the slide. In the loss state `recover` stays: the episode is measured
         * against it.
         */
        void on_ack(const AckInfo& info, const wire::TcpSequence ack) noexcept {
            assert(state_ != RecoveryState::Recovery && "the window in recovery is not the policy's to grow");
            dupacks_ = 0;
            CongestionControl::on_ack(congestion_, info);
            if (state_ == RecoveryState::Open && !holds_guard(ack, info.flight)) {
                slide(ack);
            }
        }

        // The undo's record.
        /// An episode's undo is open.
        [[nodiscard]] bool undo_open() const noexcept {
            return undo_open_;
        }

        /// A resend of the episode left, not a probe: counted, and the first one's stamp kept. Nothing while no undo is
        /// open: a new episode replaces the record before its first resend.
        ///
        /// Where this departs from Linux's `tcp_retransmit_skb`, which stamps `retrans_stamp` and counts `undo_retrans`
        /// at every attempt, one the busy rule defers included: a resend counts here once it left, so an ACK that
        /// moves SND.UNA while the first one still waits for its packet answers an original (`answers_original`), as
        /// the spec's "Undo" has it.
        void on_resend(const core::TimePoint now) noexcept {
            if (!undo_open_) {
                return;
            }
            ++unreported_;
            resends_counted_ = true;
            if (!first_resend_) {
                first_resend_ = now;
            }
        }

        /// A probe was in flight when the episode began: one more resend that may be reported.
        ///
        /// Where this departs from Linux's `tcp_init_undo`: it counts the probe while `tlp_high_seq` is set, its
        /// verdict open; here only while it is in flight. A report of a probe whose end SND.UNA already covers ends at
        /// or below the episode's start and could never count down (`on_duplicate_report`).
        void count_probe() noexcept {
            if (undo_open_) {
                ++unreported_;
                resends_counted_ = true;
            }
        }

        /// A believed duplicate report for resent data that ends above the episode's start: one resend fewer to wait
        /// for, never below none. Which reports are of resent data is the caller's to judge (`AckProcessor`): above
        /// SND.UNA, one that names a queued entry that was resent, as Linux's `tcp_sacktag_one` counts only a buffer
        /// with `TCPCB_RETRANS`; at or below it, any, of data the queue no longer holds, as `tcp_check_dsack` counts
        /// it.
        ///
        /// Where this departs from Linux, which counts down `undo_retrans` by the segments a report covers: one resend
        /// per report here, which a peer reports one segment at a time.
        void on_duplicate_report(const wire::TcpSequence end) noexcept {
            if (undo_open_ && unreported_ > 0 && end.after(episode_start_)) {
                --unreported_;
            }
        }

        /// The first sign, `tcp_try_undo_dsack`'s: resends were counted and every one was reported.
        [[nodiscard]] bool every_resend_reported() const noexcept {
            return resends_counted_ && unreported_ == 0;
        }

        /**
         * The second sign, Linux's `tcp_packet_delayed`, for an ACK that moved SND.UNA: nothing of the episode was
         * resent yet, or `echo` is older than `first_resend_value`, the timestamp value of the first resend, in the
         * serial arithmetic of timestamp values. Either is missing without timestamps, and the caller passes no echo
         * of zero. The values tick in milliseconds: an original sent in its first resend's millisecond cannot be told
         * from it, and gives no sign. Linux's two exceptions to "nothing resent yet" have nothing to guard here: the
         * first resend's stamp is cleared only where no resend of an episode is unacknowledged, as an episode begins
         * (`open_undo`) or once it ended (`forget_first_resend`), where Linux's `tcp_is_non_sack_preventing_reopen`
         * clears `retrans_stamp` within one, and no record is open in the handshake, where `tcp_clean_rtx_queue`
         * clears it.
         */
        [[nodiscard]] bool answers_original(const std::optional<std::uint32_t> echo,
                                            const std::optional<std::uint32_t> first_resend_value) const noexcept {
            if (!first_resend_) {
                return true;
            }
            return echo && first_resend_value && static_cast<std::int32_t>(*echo - *first_resend_value) < 0;
        }

        /// The stamp of the episode's first resend, for the caller to turn into a timestamp value.
        [[nodiscard]] std::optional<core::TimePoint> first_resend() const noexcept {
            return first_resend_;
        }

        /// The episode ended, or an undo opened the connection, and no resend of an episode is unacknowledged any more:
        /// the first resend's stamp is forgotten, as Linux's `tcp_try_to_open` clears `retrans_stamp` unless
        /// `tcp_any_retrans_done`. The stamp is an episode's: a later one starts its own, and a tail probe, no
        /// episode's resend, never carries this one forward. The record stays open for the reports that trail.
        void forget_first_resend() noexcept {
            first_resend_ = std::nullopt;
        }

        /**
         * The reduction is taken back, Linux's `tcp_undo_cwnd_reduction`: the policy's `on_undo` with the state copied
         * at the start, and the undo closes; nothing once it is closed. `reopen` also ends the episode: the state is
         * open and the duplicate count cleared, as `end` leaves them, but the window stays the one restored.
         *
         * `recover` stays where the episode set it. An undo that reopens short of it, on a partial ACK or in the loss
         * state, keeps RFC 6582's guard of the episode: duplicates its needless resends draw begin no recovery until an
         * ACK covers more than the highest sequence out when it began, as after a timeout that was not undone, and
         * `on_ack` does not slide it back meanwhile. At the ACK that covers it, the guard admits what `end` would.
         */
        void undo(const bool reopen) noexcept {
            if (!undo_open_) {
                return;
            }
            CongestionControl::on_undo(congestion_, prior_);
            undo_open_ = false;
            if (reopen) {
                state_   = RecoveryState::Open;
                dupacks_ = 0;
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

        /// `recover` is at or ahead of `ack`, by less than `flight`, SND.NXT - SND.UNA before the ACK: an episode's,
        /// which lies below SND.NXT, not yet covered. Ahead by more, it is a `recover` left 2^31 behind.
        [[nodiscard]] bool holds_guard(const wire::TcpSequence ack, const std::uint32_t flight) const noexcept {
            const std::int32_t ahead = recover_ - ack;
            return ahead >= 0 && static_cast<std::uint32_t>(ahead) < flight;
        }

        /**
         * An episode sets a target: the undo's record opens anew from `start`, SND.UNA, with the policy's state as it
         * is. Linux's `tcp_init_undo`, with the state `tcp_enter_recovery` and `tcp_enter_loss` copy, but as it is:
         * Linux records the old target as at least three quarters of the old window (`tcp_current_ssthresh`), which is
         * not copied.
         *
         * What earlier resends leave carries over, as in Linux. The resends still out are counted among those that
         * may be reported, as `tcp_init_undo` starts `undo_retrans` at `retrans_out`: the duplicate report of one of
         * them, which ends above `start`, would otherwise count down the episode's own resends. And while an earlier
         * episode's resend is unacknowledged, out or marked lost again, the first resend's stamp stays, as
         * `tcp_retrans_stamp_cleanup` keeps `retrans_stamp` while `tcp_any_retrans_done`: else an ACK of that resend,
         * while the episode's first waits for its packet, would read as one before any resend (`answers_original`).
         * With none unacknowledged the stamp clears, and the episode's first resend sets it; a tail probe's resend is
         * no episode's and keeps nothing (`EarlierResends::unacknowledged`).
         */
        void open_undo(const wire::TcpSequence start, const EarlierResends earlier) noexcept {
            prior_           = congestion_;
            undo_open_       = true;
            episode_start_   = start;
            unreported_      = earlier.out;
            resends_counted_ = earlier.out > 0;
            if (!earlier.unacknowledged) {
                first_resend_ = std::nullopt;
            }
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
        RateReduction rate_{};         ///< The window in recovery.
        // The undo's record.
        State prior_{};                      ///< The policy's state as the episode began: what an undo restores.
        bool undo_open_           = false;   ///< An episode's undo is open: opened as it began, closed by its undo.
        bool resends_counted_     = false;   ///< Some resend was counted: a count of none means every one was reported.
        std::uint32_t unreported_ = 0;       ///< Resends counted and not yet reported as duplicates.
        wire::TcpSequence episode_start_{};  ///< SND.UNA as the episode began.
        /// The first resend that left of the episode, or of an earlier episode whose resends were still unacknowledged
        /// as it began; nothing once an episode ended, or an undo reopened, with no resend of an episode
        /// unacknowledged.
        std::optional<core::TimePoint> first_resend_{};
    };

}  // namespace aloe::tcp::detail
