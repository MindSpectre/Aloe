#pragma once

#include <aloe/wire>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <net_frames.hpp>
#include <tcp_frames.hpp>

namespace aloe::frames {

    /**
     * @brief The other end of a connection, scripted by the test: builds each segment with the
     * right numbers from what it has seen, and never replies on its own.
     *
     * `spec` holds the peer's outbound orientation: its MACs, addresses and ports as the source,
     * the stack's as the destination. `see` is fed every frame the stack transmits and advances
     * `rcv_nxt`, `snd_una`, the stack's window and MSS. Every other member returns a frame.
     */
    class TcpPeer {
    public:
        TcpPeer(const TcpSpec spec, const wire::TcpSequence initial) noexcept
            : spec_{spec},
              iss_{initial},
              snd_nxt_{initial} {
        }

        /// SYN with the spec's MSS; takes one sequence number.
        [[nodiscard]] std::vector<std::byte> syn() {
            std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Syn}, {}, snd_nxt_, wire::TcpSequence{}, true);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// Answers the stack's SYN: `see` adopts its ports, then SYN-ACK acknowledges it and takes one sequence number.
        [[nodiscard]] std::vector<std::byte> syn_ack(const ParsedFrame& seen) {
            see(seen);
            std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, true);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// Sees `seen`, then a pure ACK of everything seen so far.
        [[nodiscard]] std::vector<std::byte> ack(const ParsedFrame& seen) {
            see(seen);
            return ack();
        }

        [[nodiscard]] std::vector<std::byte> ack() {
            return segment(wire::TcpFlags{wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
        }

        /// PSH+ACK carrying `bytes`; takes their sequence space.
        [[nodiscard]] std::vector<std::byte> data(const std::span<const std::byte> bytes) {
            std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack}, bytes, snd_nxt_, rcv_nxt_, false);
            snd_nxt_ = snd_nxt_ + static_cast<std::uint32_t>(bytes.size());
            return frame;
        }

        /// FIN+ACK; takes one sequence number.
        [[nodiscard]] std::vector<std::byte> fin() {
            std::vector<std::byte> frame =
                segment(wire::TcpFlags{wire::TcpFlag::Fin, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
            snd_nxt_ = snd_nxt_ + 1;
            return frame;
        }

        /// RST+ACK at the current numbers.
        [[nodiscard]] std::vector<std::byte> rst() {
            return segment(wire::TcpFlags{wire::TcpFlag::Rst, wire::TcpFlag::Ack}, {}, snd_nxt_, rcv_nxt_, false);
        }

        /// Any segment at explicit numbers, with no change to the peer's state: for duplicates and gaps.
        [[nodiscard]] std::vector<std::byte> segment(const wire::TcpFlags flags,
                                                     const std::span<const std::byte> payload,
                                                     const wire::TcpSequence sequence,
                                                     const wire::TcpSequence acknowledgement,
                                                     const bool with_mss = false) const {
            TcpSpec spec         = spec_;
            spec.sequence        = sequence;
            spec.acknowledgement = acknowledgement;
            spec.flags           = flags;
            if (!with_mss) {
                spec.mss = std::nullopt;
            }
            return tcp_frame(spec, payload);
        }

        /// Observes a frame the stack transmitted, without replying. A SYN sets `rcv_nxt` outright and, when it
        /// carries no ACK, makes the peer adopt its ports to answer it; after that `rcv_nxt` only advances, so a
        /// retransmission past the SYN changes nothing. Also tracks the stack's ACK, window and MSS.
        void see(const ParsedFrame& frame) noexcept {
            if (!frame.tcp) {
                return;
            }
            const wire::TcpHeader& tcp = *frame.tcp;
            auto length                = static_cast<std::uint32_t>(frame.l4.size() - tcp.data_offset);
            if (tcp.flags.has(wire::TcpFlag::Syn)) {
                ++length;
                if (!tcp.flags.has(
                        wire::TcpFlag::Ack)) {  // the stack opened: the peer answers from the SYN's destination port
                    spec_.source_port      = tcp.destination_port;
                    spec_.destination_port = tcp.source_port;
                }
                if (const auto options = wire::TcpOptions::parse(std::span<const std::byte>{frame.l4}.subspan(
                        wire::TcpHeader::size, tcp.data_offset - wire::TcpHeader::size))) {
                    stack_mss_ = options->mss;
                }
                synchronised_ = true;
                rcv_nxt_      = tcp.sequence + length;
            } else if (tcp.flags.has(wire::TcpFlag::Fin)) {
                ++length;
            }
            const wire::TcpSequence end = tcp.sequence + length;
            if (synchronised_ && end.after(rcv_nxt_)) {
                rcv_nxt_ = end;
            }
            if (tcp.flags.has(wire::TcpFlag::Ack) && tcp.acknowledgement.after(snd_una_)) {
                snd_una_ = tcp.acknowledgement;
            }
            stack_window_ = tcp.window;
        }

        [[nodiscard]] wire::TcpSequence iss() const noexcept {
            return iss_;
        }
        [[nodiscard]] wire::TcpSequence snd_nxt() const noexcept {
            return snd_nxt_;
        }
        [[nodiscard]] wire::TcpSequence rcv_nxt() const noexcept {
            return rcv_nxt_;
        }
        [[nodiscard]] wire::TcpSequence snd_una() const noexcept {
            return snd_una_;
        }
        [[nodiscard]] std::uint16_t stack_window() const noexcept {
            return stack_window_;
        }
        [[nodiscard]] std::optional<std::uint16_t> stack_mss() const noexcept {
            return stack_mss_;
        }
        [[nodiscard]] TcpSpec& spec() noexcept {
            return spec_;
        }
        [[nodiscard]] const TcpSpec& spec() const noexcept {
            return spec_;
        }

        /// Rewinds the peer's own sequence by `count` bytes, for scripting a retransmission.
        void rewind(const std::uint32_t count) noexcept {
            snd_nxt_ = wire::TcpSequence{snd_nxt_.value - count};
        }

    private:
        TcpSpec spec_;
        wire::TcpSequence iss_;
        wire::TcpSequence snd_nxt_;
        wire::TcpSequence rcv_nxt_{};
        wire::TcpSequence snd_una_{};
        std::uint16_t stack_window_ = 0;
        std::optional<std::uint16_t> stack_mss_;
        bool synchronised_ = false;
    };

}  // namespace aloe::frames
