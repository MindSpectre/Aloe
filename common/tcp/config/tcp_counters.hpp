#pragma once

#include <cstdint>

namespace aloe::tcp {

    /// One per stack, monotonic, read on the owning thread or after it has stopped.
    struct TcpCounters {
        std::uint64_t segments_received     = 0;
        std::uint64_t data_segments_sent    = 0;
        std::uint64_t pure_acks_sent        = 0;
        std::uint64_t control_segments_sent = 0;  ///< SYN, SYN-ACK, FIN, first sends and retransmits alike.
        std::uint64_t resets_sent           = 0;
        /// Challenge ACKs the limiter let go (RFC 5961): sent at once, or left for the flush when the device refused
        /// them. Each is also one of `pure_acks_sent` once it leaves.
        std::uint64_t challenge_acks_sent   = 0;
        /// Answers the limiter held back: challenge ACKs, ACKs for a refused segment that occupies no sequence space or
        /// carries a SYN, and the SYN-ACK sent again for the peer's SYN again in SynReceived.
        std::uint64_t acks_rate_limited     = 0;
        /// Resends at an expiry of the retransmission timer, of any kept segment, SYN, SYN-ACK, data and FIN: one per
        /// expiry, whether the resend left, waited or was refused.
        std::uint64_t retransmits           = 0;
        /// Resends of entries marked lost, in recovery and in the loss state after a timeout: one per entry per mark,
        /// at its first try, whether it left, waited or was refused. The expiry's own resend is `retransmits`'.
        std::uint64_t fast_retransmits      = 0;
        std::uint64_t recoveries            = 0;  ///< Episodes of recovery entered; a timeout's loss state is not one.
        /// A resend that found its packet lent and waited for the next flush: one per deferral, and one per stamp while
        /// a segment's packet and its copy both stay lent.
        std::uint64_t retransmits_deferred  = 0;
        std::uint64_t retransmits_copied    = 0;  ///< A resend sent as a copy, after the deferral bound.
        std::uint64_t zero_window_probes    = 0;
        std::uint64_t duplicate_acks        = 0;
        std::uint64_t rtt_samples           = 0;
        std::uint64_t window_updates        = 0;  ///< Segments that extended the advertised edge.
        std::uint64_t out_of_order_held     = 0;  ///< Segments held above a gap.
        std::uint64_t out_of_order_released =
            0;  ///< Held out-of-order segments released to make room for a lower sequence.
        std::uint64_t sack_acks_sent       = 0;  ///< Pure ACKs that carried SACK blocks.
        std::uint64_t duplicate_sacks_sent = 0;  ///< Of them, those whose first block reported data that arrived twice.
        std::uint64_t delayed_acks_sent    = 0;  ///< ACKs the delay timer sent.
        std::uint64_t sack_acks_received   = 0;  ///< ACKs received whose SACK option was read.
        /// Of them, those whose first block was a believed duplicate report.
        std::uint64_t duplicate_sacks_received = 0;
        std::uint64_t rack_marked              = 0;  ///< Segments RACK marked lost.
        std::uint64_t lost_retransmits         = 0;  ///< Of them, resends marked lost again.
        std::uint64_t reorder_timeouts         = 0;  ///< Expiries of the reordering timer.
        std::uint64_t reordering_seen          = 0;  ///< ACKs that showed reordering.
        /// Expiries of the retransmission timer that found the head reported received: the peer reneged, and every
        /// received mark was cleared.
        std::uint64_t sack_reneging            = 0;
        /// Tail probes sent (RFC 8985 7.3), each counted when its resend leaves: one the busy rule holds counts at the
        /// flush that sends it, and one that never leaves counts nothing.
        std::uint64_t tail_probes              = 0;
        /// Probes a duplicate report or a plain duplicate ACK found needless: both copies arrived.
        std::uint64_t tail_probes_needless     = 0;
        std::uint64_t tail_probe_recoveries    = 0;  ///< Losses a probe repaired: the window cut once, no episode.
        /// Verdicts a second probe closed as a loss before an ACK said which (`tail_probe_recoveries` counts them too):
        /// how often Aloe's departure from RFC 8985 7.3 and 7.4.2 was used.
        std::uint64_t tail_probes_early        = 0;
        /// Reductions taken back because every resend of the episode was reported as a duplicate.
        std::uint64_t undos_duplicate_sack     = 0;
        /// Reductions taken back because an ACK that moved SND.UNA answered an original: its echo is older than the
        /// episode's first resend, or nothing of the episode had been resent.
        std::uint64_t undos_original_ack       = 0;

        std::uint64_t connections_opened         = 0;  ///< Active opens that reached ESTABLISHED.
        std::uint64_t connections_accepted       = 0;
        std::uint64_t connections_closed         = 0;  ///< Normal closes, both FINs acknowledged.
        std::uint64_t connections_reset          = 0;  ///< RST received on a connection we own.
        std::uint64_t connections_timed_out      = 0;
        std::uint64_t connections_user_timed_out = 0;  ///< Of connections_timed_out, those the user timeout ended.
        std::uint64_t connections_unresolved     = 0;  ///< Owned connections whose next hop never answered ARP.
        std::uint64_t handshakes_failed          = 0;  ///< A passive open nobody owned yet was reset or timed out.
        /// Handshakes refused because the peer's MSS, stated or assumed, was below `min_peer_mss`: a SYN to a listener
        /// that took no slot, or an active open whose SYN-ACK was answered with a RST. Each drew a RST; neither moves
        /// `connections_reset`.
        std::uint64_t handshakes_refused_mss     = 0;
        /// SYNs a listener took that carried payload, which is neither held nor acknowledged: the peer sends it again
        /// after the handshake. A SYN refused before it takes a slot counts only where it was refused.
        std::uint64_t syn_payload_discarded      = 0;

        std::uint64_t dropped_bad_header    = 0;
        std::uint64_t dropped_bad_checksum  = 0;
        std::uint64_t dropped_no_connection = 0;
        std::uint64_t dropped_closed        = 0;
        std::uint64_t dropped_unexpected    = 0;
        std::uint64_t dropped_duplicate     = 0;
        std::uint64_t dropped_out_of_order  = 0;
        std::uint64_t dropped_out_of_window = 0;
        std::uint64_t dropped_no_slot       = 0;
        std::uint64_t dropped_no_node       = 0;
        std::uint64_t dropped_table_full    = 0;
        /// SYNs to a listener whose source has no next hop (`net::Ipv4::next_hop`): off the subnet with no gateway, or,
        /// with the IP brick's learning off, a neighbour outside its static table. Dropped before they take a slot,
        /// and answered with nothing, since nothing could reach the source.
        std::uint64_t dropped_no_route      = 0;
        std::uint64_t dropped_paws = 0;  ///< Segments whose timestamp was older than TS.Recent; acknowledged, not kept.
        /// RSTs inside the window at none of the positions that reset (RFC 5961 3.2): each drew a challenge ACK, or
        /// was held back by the limiter.
        std::uint64_t dropped_reset_challenged = 0;
        /// SYNs on a connection past SynReceived (RFC 5961 4.2), wherever their sequence number lay and whether or not
        /// they passed PAWS: each drew a challenge ACK, or was held back by the limiter, and moved no other drop
        /// counter.
        std::uint64_t dropped_syn_challenged   = 0;
        /// Segments whose ACK was below SND.UNA less the smaller of the largest window the peer offered and what it
        /// has acknowledged since the connection became Established (RFC 5961 5.2): dropped whole, payload and FIN
        /// included, and each drew a challenge ACK, or was held back by the limiter. An ACK above SND.NXT stays in
        /// `dropped_unexpected`.
        std::uint64_t dropped_old_ack          = 0;

        std::uint64_t send_refused        = 0;
        std::uint64_t send_unresolved     = 0;
        /// A commit after the connection left Established and CloseWait, or whose count the windows no longer cover;
        /// and a commit without a prepare, a contract violation a release build refuses.
        std::uint64_t commits_refused     = 0;
        /// A packet the pool could not supply for a prepare, or a queue node for a commit, both with a retry hint that
        /// says when to try again; and a copy fallback whose packet the pool could not supply.
        std::uint64_t allocation_failures = 0;

        friend constexpr bool operator==(const TcpCounters&, const TcpCounters&) noexcept = default;
    };

}  // namespace aloe::tcp
