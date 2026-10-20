#pragma once

#include <aloe/ethdev>
#include <optional>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /**
     * @brief Level 1: the runtime with one shard, pinned to `stack_core` when given, and one task.
     *
     * Tick round: the task accepts, then loops `readable(payload)`, fold, consume, prepare, write, arm, commit; the
     * shard's flush carries the order out, and the task finishes a step after the last one, outside its window. Gateway
     * round: the task connects and yields a step, so the handshake's ACK leaves alone, then per round stamps the start,
     * prepares, writes, arms, commits and awaits `acked(committed())`, with the gap before the next. The calling
     * thread sleeps on `state.stop` meanwhile, stops and joins the runtime, then checks the counters. Cross-core only.
     */
    void run_runtime(RunState& state,
                     StampedDevice<ethdev::Port>& device,
                     const PeerConfig& peer_config,
                     bool arp_on_wire,
                     std::optional<unsigned> stack_core);

}  // namespace aloe::benchmarks
