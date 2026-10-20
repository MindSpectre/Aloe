#pragma once

#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <clock.hpp>
#include <run_state.hpp>

namespace aloe::benchmarks {

    struct PeerConfig {
        wire::MacAddress stack_mac{};  ///< The stack port's MAC: all zeros on a ring port.
        wire::MacAddress peer_mac{0x02, 0, 0, 0, 0xfe, 0xed};
        wire::Ipv4Address stack_ip{10, 0, 0, 2};
        wire::Ipv4Address peer_ip{10, 0, 0, 1};
        std::uint16_t stack_port = 7;      ///< Where the stack listens in the tick round.
        std::uint16_t peer_port  = 40000;  ///< The peer's port; the stack connects to it in the gateway round.
        std::uint16_t mss        = 1460;
        bool strict              = true;  ///< Check checksums and sequence numbers of every order; off at the floor.
        core::Duration deadline  = std::chrono::seconds{1};  ///< The longest wait for a frame before the run fails.
    };

    struct PeerCounters {
        std::uint64_t orders_seen       = 0;
        std::uint64_t pure_acks_seen    = 0;  ///< The 1460 row's second-segment ACKs, skipped.
        std::uint64_t arp_replies_sent  = 0;
        std::uint64_t surplus_freed     = 0;  ///< Stack mbufs the peer had no use for, freed on its own core.
        std::uint64_t unexpected_frames = 0;  ///< Neither TCP nor ARP.
    };

    enum class PeerError : std::uint8_t {
        Timeout,        ///< No frame within the deadline.
        BadChecksum,    ///< An order's IPv4 or TCP checksum was wrong.
        WrongRound,     ///< An order whose first bytes are not the round awaited.
        WrongSequence,  ///< An order out of sequence.
        Unexpected,     ///< Another frame where a handshake segment was due.
    };

    [[nodiscard]] constexpr std::string_view to_string(const PeerError error) noexcept {
        switch (error) {
            case PeerError::Timeout:
                return "no frame within the deadline";
            case PeerError::BadChecksum:
                return "an order with a bad checksum";
            case PeerError::WrongRound:
                return "an order for another round";
            case PeerError::WrongSequence:
                return "an order out of sequence";
            case PeerError::Unexpected:
                return "an unexpected frame during the handshake";
        }
        return "?";
    }

    /**
     * @brief The venue, scripted: the other end of the stack's one connection, over the peer port of a RingPair.
     *
     * Every member runs on the peer's thread, or inline on the stack's thread in the same-core topology. The active
     * open's SYN and ACK, every ARP reply and, at the floor, the first tick come from the peer port's pool; every
     * other frame is written into the carrier, the mbuf the stack's last frame arrived in, so the stack core's pool
     * stays balanced. The peer answers an ARP request whenever it sees one. `starts[round]` receives the tick round's
     * start stamp, taken once the tick is in the carrier and just before it is transmitted.
     */
    class VenuePeer {
    public:
        using Packet = ethdev::Packet;

        VenuePeer(ethdev::Port& port, const PeerConfig& config, std::span<std::uint64_t> starts);

        /// Tick round, the peer opens: SYN from the pool.
        [[nodiscard]] std::expected<void, PeerError> open_active();
        /// Tick round: the SYN-ACK, kept as the carrier, then the ACK from the pool.
        [[nodiscard]] std::expected<void, PeerError> finish_active_open();
        /// Gateway round, the stack opens: the SYN-ACK, written into the SYN's own mbuf and sent back in it.
        [[nodiscard]] std::expected<void, PeerError> answer_passive_open();
        /// Gateway round: the handshake's ACK, freed.
        [[nodiscard]] std::expected<void, PeerError> finish_passive_open();

        /// Builds tick `round` of `payload` bytes into the carrier, stamps `starts[round]`, transmits. A tick the
        /// carrier cannot hold goes unstamped; a tick that never arrives fails its round at the deadline.
        void send_tick(std::uint64_t round, std::size_t payload);
        /// Polls for the order of `round`: skips and frees pure ACKs, answers ARP, checks the order, keeps its mbuf
        /// as the carrier. Fails on the deadline or on a wrong order.
        [[nodiscard]] std::expected<void, PeerError> await_order(std::uint64_t round);
        /// Gateway round: a bare ACK of everything seen, in the carrier.
        void send_bare_ack();

        [[nodiscard]] const PeerCounters& counters() const noexcept {
            return counters_;
        }

        [[nodiscard]] const frames::TcpPeer& script() const noexcept {
            return script_;
        }

    private:
        /// The next frame within the deadline, ARP requests answered and dropped on the way; nothing on the deadline.
        [[nodiscard]] std::optional<Packet> collect();
        /// A fresh packet from the pool carrying `frame`; a refused transmit is counted by the round that times out.
        void inject(std::span<const std::byte> frame);
        /// `frame` into the carrier, which must be present, in place of what it held. False when it has no room.
        [[nodiscard]] bool prepare_carrier(std::span<const std::byte> frame);
        /// The prepared carrier out; a refused transmit keeps it as the carrier.
        void send_carrier() noexcept;
        void answer_arp(const frames::ParsedFrame& request);
        [[nodiscard]] static bool is_pure_ack(const frames::ParsedFrame& parsed) noexcept;

        ethdev::Port* port_;
        PeerConfig config_;
        frames::TcpPeer script_;
        std::span<std::uint64_t> starts_;
        Packet carrier_;
        std::vector<std::byte> tick_;  ///< The last tick's frame, kept so no free falls between its stamps.
        std::array<Packet, 16> burst_{};
        std::size_t pending_ = 0;  ///< Frames in `burst_` not yet taken: `[next_, pending_)`.
        std::size_t next_    = 0;
        PeerCounters counters_;
    };

    /// The peer's side of every round of `state`, on the calling thread: the handshake when `handshake`, then the
    /// tick or gateway rounds, honouring `state.config.gap` and failing `state` on the first error.
    void drive_peer(VenuePeer& peer, RunState& state, bool handshake);

}  // namespace aloe::benchmarks
