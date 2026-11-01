#pragma once

#include <cstdint>

#include <tcp_config.hpp>
#include <tcp_header.hpp>
#include <tcp_sequence.hpp>

namespace aloe::tcp::detail {

    /**
     * @brief RFC 793's send sequence variables, and the peer's MSS: what the sender numbered and what the peer's ACKs
     * told it.
     */
    struct SendSequence {
        wire::TcpSequence iss{};  ///< ISS.
        wire::TcpSequence una{};  ///< SND.UNA: everything before it reached the peer.
        wire::TcpSequence nxt{};  ///< SND.NXT: the sequence after the last committed byte, or the FIN.
        std::uint32_t wnd = 0;    ///< SND.WND: the peer's window, from `una`.
        wire::TcpSequence wl1{};  ///< SND.WL1: the sequence of the segment that last updated `wnd`.
        wire::TcpSequence wl2{};  ///< SND.WL2: its acknowledgement.
        std::uint16_t peer_mss = default_peer_mss;
        bool fin_sent          = false;  ///< The FIN took its number, `nxt - 1`; its entry carries it.

        /// Numbers the SYN: `iss` is unacknowledged and the SYN takes it.
        void open(const wire::TcpSequence isn) noexcept {
            iss = isn;
            una = isn;
            nxt = isn + 1;
        }

        /// The handshake's window: SND.WND from the peer's SYN or SYN-ACK, which also sets WL1 and WL2.
        void synchronize(const wire::TcpHeader& header) noexcept {
            wnd = header.window;
            wl1 = header.sequence;
            wl2 = header.acknowledgement;
        }

        /// RFC 793's window update rule: a newer segment, or the same one with a newer acknowledgement.
        void update_window(const wire::TcpHeader& header) noexcept {
            if (wl1.before(header.sequence) || (wl1 == header.sequence && !wl2.after(header.acknowledgement))) {
                synchronize(header);
            }
        }

        /// Sequence space sent, or queued to be, and not acknowledged.
        [[nodiscard]] std::uint32_t flight() const noexcept {
            return nxt.value - una.value;
        }

        /// RFC 9293's send window test, Linux's `tcp_snd_wnd_test`: the peer's window covers a segment ending at `end`.
        [[nodiscard]] bool covers(const wire::TcpSequence end) const noexcept {
            return !end.after(una + wnd);
        }
    };

}  // namespace aloe::tcp::detail
