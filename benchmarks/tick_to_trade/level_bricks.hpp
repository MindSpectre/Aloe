#pragma once

#include <aloe/ethdev>
#include <aloe/net>
#include <chrono>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /// The IP brick's configuration at every level: the stack's address, and an ARP cache whose entries outlive any
    /// run, so no refresh request lands on the hot path.
    [[nodiscard]] inline net::Ipv4Config stack_ip_config(const PeerConfig& peer) noexcept {
        return {.address       = peer.stack_ip,
                .prefix        = 24,
                .arp_reachable = std::chrono::hours{24},
                .arp_expire    = std::chrono::hours{48}};
    }

    /**
     * @brief Level 0: the hand-written loop over the bricks, `examples/tcp_echo`'s loop with the application in the
     * event drain.
     *
     * Tick round: the stack listens, the peer opens; on Readable with the whole tick unread, fold it, consume it,
     * prepare, write the order, arm, commit; `tcp.flush` and `queue.flush` carry it out. Gateway round: the stack
     * connects; from the step after Connected, so the handshake's ACK leaves alone, and after each order is
     * acknowledged and the gap has passed, stamp the start, prepare, write, arm, commit. `--breakdown` stamps the
     * phases of the tick round (`Phase`); the gateway round records the Application and TcpFlush phases only. Runs on
     * the calling thread until every round is stamped or `state` stops, then checks the counters. With `inline_peer`
     * the peer's side runs inline between steps (same-core); ARP on the wire needs the peer on its own thread, since
     * the peer answers a request only while it waits for a frame.
     */
    void run_bricks(RunState& state,
                    StampedDevice<ethdev::Port>& device,
                    const PeerConfig& peer_config,
                    bool arp_on_wire,
                    VenuePeer* inline_peer);

}  // namespace aloe::benchmarks
