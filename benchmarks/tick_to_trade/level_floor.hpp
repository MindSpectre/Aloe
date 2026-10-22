#pragma once

#include <aloe/ethdev>
#include <cstddef>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /// Ethernet, IPv4 and TCP headers without options: where a tick's or an order's payload starts in the frame.
    inline constexpr std::size_t frame_header_size = 14 + 20 + 20;

    /**
     * @brief The floor: nothing between the device and the application.
     *
     * Tick round: receive the raw frame, fold its payload, write the order into a fresh packet from a pre-built frame
     * template, arm, transmit. Gateway round: stamp, write the order, arm, transmit, then receive and drop the peer's
     * bare ACK before the next round. Runs on the calling thread until every round is stamped, then finishes
     * `state`, or until `state` stops. With `inline_peer` the peer's side of each round runs inline before and after
     * the stack's (same-core); without it a `drive_peer` on another thread does it. Whoever starts a round waits
     * `state.config.gap` after the last one ended, and a round that outlasts `peer_config.deadline` fails `state`.
     */
    void run_floor(RunState& state,
                   StampedDevice<ethdev::Port>& device,
                   const PeerConfig& peer_config,
                   VenuePeer* inline_peer);

}  // namespace aloe::benchmarks
