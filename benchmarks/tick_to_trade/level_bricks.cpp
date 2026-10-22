#include "level_bricks.hpp"

#include <aloe/loop>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <application_work.hpp>
#include <clock.hpp>
#include <stack_sanity.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Device = StampedDevice<ethdev::Port>;
        using Ipv4   = net::Ipv4<Device>;
        using Tcp    = tcp::Stack<Ipv4>;
        using Packet = ethdev::Packet;

        /// The bricks of one shard and the loop over them, with the two applications.
        class BricksLoop {
        public:
            BricksLoop(RunState& state, Device& device, const PeerConfig& peer_config, const bool arp_on_wire)
                : state_{&state},
                  device_{&device},
                  queue_{device, 0, 512, counters_},
                  ip_{queue_, stack_ip_config(peer_config)},
                  wheel_{std::chrono::milliseconds{1}, core::Clock::now()},
                  tcp_{ip_, wheel_, {}},
                  deadline_{peer_config.deadline + state.config.gap},  // the gap is silence by design, not a loss
                  progress_{core::Clock::now()} {
                if (!arp_on_wire) {
                    ip_.learn(peer_config.peer_ip, peer_config.peer_mac, core::Clock::now());
                }
                if (state.config.round == Round::Tick) {
                    if (!tcp_.listen(peer_config.stack_port)) {
                        state.fail("level 0: listen refused");
                    }
                } else {
                    const auto opened = tcp_.connect({.address = peer_config.peer_ip, .port = peer_config.peer_port},
                                                     core::Clock::now());
                    if (!opened) {
                        state.fail(std::format("level 0: connect refused ({})", static_cast<int>(opened.error())));
                    }
                }
            }

            /// One iteration of the loop. Returns once every round is out, or on a failure.
            void step() {
                const core::TimePoint now      = core::Clock::now();
                const bool breakdown           = state_->config.breakdown;
                // The gateway round's window opens at the application, after the receive path: it records no
                // receive phases, which here would stamp the processing of the last bare ACK, before its start.
                const bool receive_phases      = breakdown && state_->config.round == Round::Tick;
                const std::size_t received     = queue_.receive(burst_);
                const std::uint64_t t_received = receive_phases && received > 0 ? core::TscClock::get_ticks() : 0;
                ip_.process(std::span<Packet>{burst_}.first(received), now);
                const std::uint64_t t_ip = receive_phases && received > 0 ? core::TscClock::get_ticks() : 0;
                tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now);
                const std::uint64_t t_tcp = receive_phases && received > 0 ? core::TscClock::get_ticks() : 0;
                std::ignore               = wheel_.advance(now);
                std::optional<std::uint64_t> stamped;
                bool progressed = false;
                bool opened     = false;
                while (const auto event = tcp_.poll_event()) {
                    progressed                  = true;
                    Tcp::ConnectionType& c      = *event->connection;
                    const stream::Events events = event->events;
                    if (events.accepted() || events.connected()) {
                        connection_ = &c;
                        opened      = true;
                    }
                    if (events.readable() && state_->config.round == Round::Tick) {
                        stamped = answer_tick(c);
                    }
                    if (events.closed() || events.reset() || events.timed_out() || events.unresolved()) {
                        state_->fail(std::format("level 0: the connection ended, events {:#x}", events.raw()));
                        return;
                    }
                }
                // The handshake's ACK leaves alone in the flush of the step that connected, as the peer's script
                // expects; an order in that step would carry it. The first order goes in the next step.
                if (state_->config.round == Round::Gateway && connection_ != nullptr && !opened) {
                    stamped = issue_order(now);
                }
                const std::uint64_t t_application = breakdown && stamped ? core::TscClock::get_ticks() : 0;
                tcp_.flush(now);
                const std::uint64_t t_flush = breakdown && stamped ? core::TscClock::get_ticks() : 0;
                std::ignore                 = queue_.flush();
                if (breakdown && stamped) {
                    auto& phases                                                   = state_->stamps.phases;
                    phases[static_cast<std::size_t>(Phase::Received)][*stamped]    = t_received;
                    phases[static_cast<std::size_t>(Phase::Ip)][*stamped]          = t_ip;
                    phases[static_cast<std::size_t>(Phase::Tcp)][*stamped]         = t_tcp;
                    phases[static_cast<std::size_t>(Phase::Application)][*stamped] = t_application;
                    phases[static_cast<std::size_t>(Phase::TcpFlush)][*stamped]    = t_flush;
                }
                if (finishing_) {
                    finishing_ = false;
                    state_->finish();  // the last order is out: the store and the wake fall outside its window
                }
                // The deadline reuses the step's own stamp, so an idle poll reads no clock but the loop's one; it
                // covers only a silent peer.
                if (progressed) {
                    progress_ = now;
                } else if (now - progress_ > deadline_) {
                    state_->fail("level 0: nothing happened within the deadline");
                }
            }

            [[nodiscard]] bool acknowledged() const noexcept {
                return connection_ != nullptr && !order_in_flight_;
            }

            [[nodiscard]] std::uint64_t frames_transmitted() const noexcept {
                return counters_.frames_transmitted;
            }

            [[nodiscard]] bool connected() const noexcept {
                return connection_ != nullptr;
            }

            void check() {
                check_stack_counters(*state_, tcp_.counters(), ip_.counters(), counters_);
            }

        private:
            /// The tick-to-trade application: the whole tick must be unread; returns the round it answered.
            [[nodiscard]] std::optional<std::uint64_t> answer_tick(Tcp::ConnectionType& c) {
                const auto view = c.unread();
                if (view.size() < state_->config.payload) {
                    return std::nullopt;  // a partial tick: not in this benchmark, but never a wrong answer
                }
                const std::uint64_t round  = read_round(view.front());
                state_->sink              ^= fold(view);
                c.consume(view.size());
                const auto out = c.prepare(order_size);
                if (!out) {
                    state_->fail(std::format("level 0, round {}: prepare refused, writable {}", round, c.writable()));
                    return std::nullopt;
                }
                write_order(*out, round);
                device_->arm(round);
                if (!c.commit(order_size)) {
                    state_->fail(std::format("level 0, round {}: commit refused", round));
                    return std::nullopt;
                }
                if (round + 1 == state_->config.total()) {
                    finishing_ = true;  // finished once this step's flush has carried the order out
                }
                return round;
            }

            /// The gateway application: one order in flight; the next after its ACK and the gap, both timed by the
            /// step's stamp `now`.
            [[nodiscard]] std::optional<std::uint64_t> issue_order(const core::TimePoint now) {
                if (order_in_flight_ && connection_->acknowledged() == connection_->committed()) {
                    order_in_flight_ = false;
                    resume_at_       = now + state_->config.gap;
                    if (next_round_ == state_->config.total()) {
                        finishing_ = true;
                    }
                }
                if (order_in_flight_ || next_round_ == state_->config.total() || now < resume_at_) {
                    return std::nullopt;
                }
                const std::uint64_t round   = next_round_++;
                state_->stamps.start[round] = core::TscClock::get_ticks();
                const auto out              = connection_->prepare(order_size);
                if (!out) {
                    state_->fail(std::format("level 0, round {}: prepare refused", round));
                    return std::nullopt;
                }
                write_order(*out, round);
                device_->arm(round);
                if (!connection_->commit(order_size)) {
                    state_->fail(std::format("level 0, round {}: commit refused", round));
                    return std::nullopt;
                }
                order_in_flight_ = true;
                return round;
            }

            RunState* state_;
            Device* device_;
            loop::ShardCounters counters_;
            loop::ShardQueue<Device> queue_;
            Ipv4 ip_;
            loop::TimerWheel wheel_;
            Tcp tcp_;
            std::vector<Packet> burst_       = std::vector<Packet>(ethdev::Port::max_burst);
            Tcp::ConnectionType* connection_ = nullptr;
            bool order_in_flight_            = false;
            bool finishing_                  = false;  ///< The last round is out; `finish` after the flush.
            std::uint64_t next_round_        = 0;
            core::Duration deadline_;
            core::TimePoint resume_at_ = {};
            core::TimePoint progress_;
        };

        /// Same-core: the peer's side between steps, with the handshake split so the stack can answer each segment.
        void run_inline(RunState& state, Device& device, BricksLoop& loop, VenuePeer& peer) {
            const auto until = [&state, &loop](const auto& done) {
                while (!done() && !state.stop.load()) {
                    loop.step();
                }
            };
            const auto fail = [&state](const PeerError error, const std::uint64_t round) {
                state.fail(std::format("peer, round {}: {}", round, to_string(error)));
            };
            const auto fail_handshake = [&state](const PeerError error) {
                state.fail(std::format("peer, handshake: {}", to_string(error)));
            };
            // Before every call into the peer: a failed run, the loop's constructor or a step, has nothing to answer,
            // and the peer would only spend its deadline waiting.
            const auto failed = [&state] { return state.failed.load(); };
            if (failed()) {
                return;
            }
            if (state.config.round == Round::Tick) {
                if (const auto opened = peer.open_active(); !opened) {
                    fail_handshake(opened.error());
                    return;
                }
                until([&loop] { return loop.frames_transmitted() >= 1; });  // the SYN-ACK is out
                if (failed()) {
                    return;
                }
                if (const auto opened = peer.finish_active_open(); !opened) {
                    fail_handshake(opened.error());
                    return;
                }
                until([&loop] { return loop.connected(); });
                const std::uint64_t gap_ticks = ticks_for(state.config.gap);
                std::uint64_t resume_at       = 0;
                for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                    while (core::TscClock::get_ticks() < resume_at) {}
                    peer.send_tick(round, state.config.payload);
                    until([&device, round] { return device.stamps_taken() > round; });
                    if (failed()) {
                        return;
                    }
                    if (const auto seen = peer.await_order(round); !seen) {
                        fail(seen.error(), round);
                        return;
                    }
                    resume_at = core::TscClock::get_ticks() + gap_ticks;
                }
                return;
            }
            until([&loop] { return loop.frames_transmitted() >= 1; });  // the SYN is out
            if (failed()) {
                return;
            }
            if (const auto opened = peer.answer_passive_open(); !opened) {
                fail_handshake(opened.error());
                return;
            }
            // Connected raised, the handshake's ACK out alone in that step's flush.
            until([&loop] { return loop.connected(); });
            if (failed()) {
                return;
            }
            if (const auto opened = peer.finish_passive_open(); !opened) {
                fail_handshake(opened.error());
                return;
            }
            for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                // The step that takes the last bare ACK issues this order once the gap has passed, so the wait is for
                // the order itself: with no gap the ACK and the next order share a step.
                until([&device, round] { return device.stamps_taken() > round; });
                if (failed()) {
                    return;
                }
                if (const auto seen = peer.await_order(round); !seen) {
                    fail(seen.error(), round);
                    return;
                }
                peer.send_bare_ack();
            }
            until([&loop] { return loop.acknowledged(); });  // the step that takes the last ACK finishes the run
        }

    }  // namespace

    void run_bricks(RunState& state,
                    Device& device,
                    const PeerConfig& peer_config,
                    const bool arp_on_wire,
                    VenuePeer* const inline_peer) {
        BricksLoop loop{state, device, peer_config, arp_on_wire};
        if (inline_peer != nullptr) {
            run_inline(state, device, loop, *inline_peer);
        } else {
            while (!state.stop.load()) {
                loop.step();
            }
        }
        loop.check();
    }

}  // namespace aloe::benchmarks
