#include "level_runtime.hpp"

#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>

#include <application_work.hpp>
#include <clock.hpp>
#include <level_bricks.hpp>
#include <stack_sanity.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Device  = StampedDevice<ethdev::Port>;
        using Stack   = runtime::TcpStack<Device>;
        using Runtime = runtime::Runtime<Device, Stack>;

        /// The tick-to-trade application as a task.
        runtime::task<void> trade(Stack::StreamsType* streams,
                                  const runtime::Scheduler scheduler,
                                  Device* device,
                                  RunState* state,
                                  const std::uint16_t port) {
            auto stream = co_await streams->accept(port);
            if (!stream) {
                state->fail(std::format("level 1: accept failed, stream error {}", static_cast<int>(stream.error())));
                co_return;
            }
            const std::size_t total = state->config.total();
            for (;;) {
                const auto readable = co_await stream->readable(state->config.payload);
                if (!readable) {
                    state->fail(
                        std::format("level 1: readable failed, stream error {}", static_cast<int>(readable.error())));
                    co_return;
                }
                const auto view            = stream->unread();
                const std::uint64_t round  = read_round(view.front());
                state->sink               ^= fold(view);
                stream->consume(view.size());
                const auto out = stream->prepare(order_size);
                if (!out) {
                    state->fail(std::format("level 1, round {}: prepare refused", round));
                    co_return;
                }
                write_order(*out, round);
                device->arm(round);
                if (!stream->commit(order_size)) {
                    state->fail(std::format("level 1, round {}: commit refused", round));
                    co_return;
                }
                if (round + 1 == total) {
                    break;
                }
            }
            // The last order leaves in this step's flush. Finishing in the next step keeps the store, the wake and
            // the release's RST, which the stream's destructor sends, out of its window.
            co_await scheduler.schedule();
            state->finish();
        }

        /// The gateway application as a task.
        runtime::task<void> orders(Stack::StreamsType* streams,
                                   const runtime::Scheduler scheduler,
                                   Device* device,
                                   RunState* state,
                                   const tcp::Endpoint venue) {
            auto stream = co_await streams->connect(venue);
            if (!stream) {
                state->fail(std::format("level 1: connect failed, stream error {}", static_cast<int>(stream.error())));
                co_return;
            }
            // The handshake's ACK leaves in the flush of the step that delivered Connected, as the peer's script
            // expects; an order committed in that step would carry it. The first order goes in the next step.
            co_await scheduler.schedule();
            const std::uint64_t gap_ticks = ticks_for(state->config.gap);
            std::uint64_t resume_at       = 0;
            for (std::uint64_t round = 0; round < state->config.total(); ++round) {
                while (core::TscClock::get_ticks() < resume_at) {}
                state->stamps.start[round] = core::TscClock::get_ticks();
                const auto out             = stream->prepare(order_size);
                if (!out) {
                    state->fail(std::format("level 1, round {}: prepare refused", round));
                    co_return;
                }
                write_order(*out, round);
                device->arm(round);
                if (!stream->commit(order_size)) {
                    state->fail(std::format("level 1, round {}: commit refused", round));
                    co_return;
                }
                const auto acked = co_await stream->acked(stream->committed());
                if (!acked) {
                    state->fail(std::format(
                        "level 1, round {}: acked failed, stream error {}", round, static_cast<int>(acked.error())));
                    co_return;
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            }
            state->finish();
        }

    }  // namespace

    void run_runtime(RunState& state,
                     Device& device,
                     const PeerConfig& peer_config,
                     const bool arp_on_wire,
                     const std::optional<unsigned> stack_core) {
        Runtime runtime{
            {.shard       = {.idle = runtime::IdlePolicy::Spin},
             .threads     = {{.cpu = stack_core, .name = "aloe-bench-shard"}},
             .thread_hook = ethdev::register_thread},
            device,
            stack_ip_config(peer_config),
            tcp::TcpConfig{}
        };
        if (!arp_on_wire) {
            runtime.shard(0).stack().ip().learn(peer_config.peer_ip, peer_config.peer_mac, core::Clock::now());
        }
        Stack::StreamsType* streams = &runtime.shard(0).stack().streams();
        if (state.config.round == Round::Tick) {
            // The peer's SYN may already wait in the ring, and a step receives before it runs the task whose accept
            // would listen, so the brick would reset it. The listener opens here, before the first step, as level 0's
            // does before its loop; the task's accept finds it.
            if (const auto listening = streams->ensure_listener(peer_config.stack_port); !listening) {
                state.fail(
                    std::format("level 1: listen refused, stream error {}", static_cast<int>(listening.error())));
                return;
            }
        }
        runtime.start();
        if (state.config.round == Round::Tick) {
            runtime.spawn(0, trade(streams, runtime.scheduler(0), &device, &state, peer_config.stack_port));
        } else {
            runtime.spawn(0,
                          orders(streams,
                                 runtime.scheduler(0),
                                 &device,
                                 &state,
                                 {.address = peer_config.peer_ip, .port = peer_config.peer_port}));
        }
        state.stop.wait(false);
        runtime.stop();
        runtime.join();
        check_stack_counters(state,
                             runtime.shard(0).stack().tcp().counters(),
                             runtime.shard(0).stack().ip().counters(),
                             runtime.counters(0));
    }

}  // namespace aloe::benchmarks
