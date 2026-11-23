#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>

// Two real bricks on two fabric ports, one thread, a hand-written loop each: the echo the docs lead with,
// with no runtime, no scripted peer and every counter accounted for.
namespace {

    using namespace std::chrono_literals;
    using Ipv4      = aloe::net::Ipv4<aloe::fabric::Port>;
    using Config    = aloe::tcp::TcpConfig;
    using TimePoint = aloe::core::TimePoint;
    using aloe::fabric::Action;
    using aloe::fabric::Egress;
    using aloe::fabric::Verdict;
    using aloe::tcp::RecoveryState;
    using aloe::wire::TcpSequence;

    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 10};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 11};
    constexpr std::size_t message          = 5000;  ///< More than three segments at MSS 1460.
    /// Each egress's delay in the cases of loss recovery: a round trip of 10 ms between the two stacks.
    constexpr aloe::core::Duration one_way = 5ms;

    /// The frame parsed, when it is a TCP segment that carries payload.
    [[nodiscard]] std::optional<aloe::frames::ParsedFrame> data_segment(const std::span<const std::byte> frame) {
        std::optional<aloe::frames::ParsedFrame> parsed = aloe::frames::parse_frame(frame);
        if (!parsed || !parsed->tcp || aloe::frames::tcp_payload(*parsed).empty()) {
            return std::nullopt;
        }
        return parsed;
    }

    /// One brick with its loop state: what a program writes once per shard.
    template <typename CongestionControl>
    struct BasicBrick {
        using Tcp = aloe::tcp::Stack<Ipv4, CongestionControl>;

        BasicBrick(aloe::fabric::Port& port, const aloe::wire::Ipv4Address address, const Config& config)
            : queue{
                  port, 0, 64, counters
        },
              ip{queue, {.address = address, .prefix = 24}}, tcp{ip, wheel, config} {
        }

        aloe::loop::ShardQueueCounters counters;
        aloe::loop::ShardQueue<aloe::fabric::Port> queue;
        Ipv4 ip;
        aloe::loop::TimerWheel wheel{1ms, TimePoint{}};
        Tcp tcp;
        std::vector<aloe::fabric::Packet> burst = std::vector<aloe::fabric::Packet>(64);

        /// Receive, process, timers: the first half of a tick; the test drains events in between, then `flush`.
        void receive(const TimePoint now) {
            const std::size_t received = queue.receive(burst);
            ip.process(std::span<aloe::fabric::Packet>{burst}.first(received), now);
            tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
            std::ignore = wheel.advance(now);
        }

        void flush(const TimePoint now) {
            tcp.flush(now);
            std::ignore = queue.flush();
        }
    };

    /// The echo's own copy: unread bytes go back out through prepare and commit, in place on the way out.
    template <typename Connection>
    void echo(Connection& c) {
        while (!c.unread().empty()) {
            const std::span<const std::byte> chunk = c.unread().front();
            const auto out                         = c.prepare(chunk.size());
            if (!out) {
                return;  // resume on Writable
            }
            std::ranges::copy(chunk.first(out->size()), out->begin());
            if (!c.commit(out->size())) {
                return;
            }
            c.consume(out->size());
        }
    }

    using Brick = BasicBrick<aloe::tcp::NewReno>;

    template <typename CongestionControl>
    class BasicTwoStacks : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {
    protected:
        using Tcp = BasicBrick<CongestionControl>::Tcp;

        explicit BasicTwoStacks(const Config& server_config = {}, const Config& client_config = {})
            : server_{server_port_, server_ip, server_config},
              client_{client_port_, client_ip, client_config} {
        }

        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_port_ =
            fabric_.add_port({.mac = server_mac, .pool_size = 256, .offloads = GetParam()});
        aloe::fabric::Port& client_port_ =
            fabric_.add_port({.mac = client_mac, .pool_size = 256, .offloads = GetParam()});
        BasicBrick<CongestionControl> server_;
        BasicBrick<CongestionControl> client_;
        TimePoint now_{};
        std::optional<TimePoint> dropped_at_{};  ///< The stamp of the last drop a client script made.
        std::optional<TimePoint> echoed_at_{};   ///< When `run_echo`'s client had every byte back.
        std::vector<std::byte> served_;          ///< What `tick` read on the server, in order.

        /// Both egresses `one_way` late, so a round trip is 10 ms. Set before the handshake, it gives the first sample
        /// too: the server's is 10 ms; the client's 11 ms, its SYN stamped at `connect` and leaving at the next flush.
        void delay_both_ways() {
            server_port_.set_impairment({.delay = one_way});
            client_port_.set_impairment({.delay = one_way});
        }

        /// As `delay_both_ways`, and the client's data segments counted in `lost`, from one, are dropped, each once;
        /// `dropped_at_` is the stamp of the last.
        void drop_client_data(std::vector<std::size_t> lost) {
            server_port_.set_impairment({.delay = one_way});
            client_port_.set_impairment({.delay  = one_way,
                                         .script = [this, lost = std::move(lost), counted = std::size_t{0}](
                                                       const Egress& egress) mutable -> std::optional<Verdict> {
                                             if (!data_segment(egress.frame) ||
                                                 !std::ranges::contains(lost, ++counted)) {
                                                 return std::nullopt;
                                             }
                                             dropped_at_ = now_;
                                             return Verdict{.action = Action::Drop};
                                         }});
        }

        /// From now on the first transmission of each client data segment that starts at one of `firsts` takes
        /// `verdict`; every other client frame stays `one_way` late. A drop's stamp is `dropped_at_`.
        void script_client_segments(std::vector<TcpSequence> firsts, const Verdict verdict) {
            client_port_.set_impairment({.delay  = one_way,
                                         .script = [this, firsts = std::move(firsts), verdict](
                                                       const Egress& egress) mutable -> std::optional<Verdict> {
                                             const std::optional<aloe::frames::ParsedFrame> segment =
                                                 data_segment(egress.frame);
                                             if (!segment) {
                                                 return std::nullopt;
                                             }
                                             const auto found = std::ranges::find(firsts, segment->tcp->sequence);
                                             if (found == firsts.end()) {
                                                 return std::nullopt;
                                             }
                                             firsts.erase(found);
                                             if (verdict.action == Action::Drop) {
                                                 dropped_at_ = now_;
                                             }
                                             return verdict;
                                         }});
        }

        /// One tick of 1 ms: the fabric steps, the server reads whatever arrived into `served_`, then the client's
        /// events are drained and `client` runs, before the client's flush.
        template <std::invocable Work>
        void tick(const Work& client) {
            now_ += 1ms;
            fabric_.advance(now_);
            server_.receive(now_);
            while (const auto event = server_.tcp.poll_event()) {
                auto& s = *event->connection;
                for (const std::span<const std::byte> chunk : s.unread()) {
                    served_.insert(served_.end(), chunk.begin(), chunk.end());
                }
                s.consume(s.unread().size());
            }
            server_.flush(now_);
            client_.receive(now_);
            while (client_.tcp.poll_event()) {}
            client();
            client_.flush(now_);
        }

        void tick() {
            tick([] {});
        }

        /// 2% loss and 10% reordering on both egresses, each `one_way` late, from fixed seeds. A frame the model
        /// reorders is held until the next advance, undelayed, so it arrives ahead of the frames sent with it.
        void lose_and_reorder_both_ways() {
            client_port_.set_impairment({.loss    = 0.02,
                                         .reorder = 0.1,
                                         .delay   = one_way,
                                         .seed    = 19});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the run
            server_port_.set_impairment({.loss    = 0.02,
                                         .reorder = 0.1,
                                         .delay   = one_way,
                                         .seed    = 23});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the run
        }

        /// Learns both MACs, listens, connects, and ticks until both ends are Established. Null on failure. Over a path
        /// `one_way` late each way the server accepts at the 16th tick.
        [[nodiscard]] typename Tcp::ConnectionType* establish(typename Tcp::ConnectionType*& client) {
            server_.ip.learn(client_ip, client_mac, now_);
            client_.ip.learn(server_ip, server_mac, now_);
            if (!server_.tcp.listen(7).has_value()) {
                return nullptr;
            }
            const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
            if (!session.has_value()) {
                return nullptr;
            }
            client                               = *session;
            typename Tcp::ConnectionType* server = nullptr;
            for (int tick = 0; tick < 100 && (server == nullptr || client->state() != aloe::tcp::State::Established);
                 ++tick) {
                now_ += 1ms;
                fabric_.advance(now_);
                server_.receive(now_);
                while (const auto event = server_.tcp.poll_event()) {
                    if (event->events.accepted()) {
                        server = event->connection;
                    }
                }
                server_.flush(now_);
                client_.receive(now_);
                while (client_.tcp.poll_event()) {}
                client_.flush(now_);
            }
            return client->state() == aloe::tcp::State::Established ? server : nullptr;
        }

        /// The client's segments cross to the server, which drains its events without consuming; its ACKs cross back.
        void exchange() {
            client_.flush(now_);
            now_ += 1ms;
            fabric_.advance(now_);
            server_.receive(now_);
            while (server_.tcp.poll_event()) {}
            server_.flush(now_);
            now_ += 1ms;
            fabric_.advance(now_);
            client_.receive(now_);
            while (client_.tcp.poll_event()) {}
        }

        /// Connects, sends `bytes` in sends of at most `chunk`, has the server echo them back and both sides close,
        /// within `ticks` of 1 ms. Returns whether both closed and the echo matched; `echoed_at_` is when it matched.
        [[nodiscard]] bool run_echo(const std::size_t bytes,
                                    const int ticks,
                                    const std::size_t chunk = std::numeric_limits<std::size_t>::max()) {
            server_.ip.learn(client_ip, client_mac, now_);
            client_.ip.learn(server_ip, server_mac, now_);
            if (!server_.tcp.listen(7).has_value()) {
                return false;
            }
            const auto sent = aloe::frames::pattern(bytes);
            std::vector<std::byte> echoed;
            std::size_t offered = 0;
            bool server_done    = false;
            bool client_done    = false;

            const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
            if (!session.has_value()) {
                return false;
            }
            typename Tcp::ConnectionType& c = **session;

            for (int tick = 0; tick < ticks && !(server_done && client_done); ++tick) {
                now_ += 1ms;
                fabric_.advance(now_);
                server_.receive(now_);
                while (const auto event = server_.tcp.poll_event()) {
                    auto& s = *event->connection;
                    if (event->events.readable() || event->events.writable()) {
                        echo(s);
                    }
                    if (s.peer_closed() && s.unread().empty() && s.state() == aloe::tcp::State::CloseWait) {
                        s.close();
                    }
                    if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
                        s.release();
                        server_done = true;
                    }
                }
                server_.flush(now_);

                client_.receive(now_);
                while (const auto event = client_.tcp.poll_event()) {
                    if (event->events.connected() || event->events.writable()) {
                        while (offered < bytes) {
                            const std::size_t accepted = c.send(
                                std::span<const std::byte>{sent}.subspan(offered, std::min(chunk, bytes - offered)));
                            if (accepted == 0) {
                                break;  // resume on Writable
                            }
                            offered += accepted;
                        }
                    }
                    if (event->events.readable()) {
                        for (const std::span<const std::byte> read : c.unread()) {
                            echoed.insert(echoed.end(), read.begin(), read.end());
                        }
                        c.consume(c.unread().size());
                        if (echoed.size() == bytes) {
                            echoed_at_ = now_;
                            c.close();
                        }
                    }
                    if (event->events.closed() || event->events.reset() || event->events.timed_out()) {
                        c.release();
                        client_done = true;
                    }
                }
                client_.flush(now_);
            }
            return server_done && client_done && echoed == sent && offered == bytes;
        }
    };

    class TcpTwoStacks : public BasicTwoStacks<aloe::tcp::NewReno> {
    protected:
        explicit TcpTwoStacks(const Config& server_config = {})
            : BasicTwoStacks(server_config) {
        }
    };

    class TcpTwoStacksNoCongestion : public BasicTwoStacks<aloe::tcp::NoCongestionControl> {};

    /// A client whose send cap holds 128 full segments, more than an unscaled window allows, and no congestion
    /// window to stop it first: only the server's scaled window limits what it puts in flight.
    class TcpTwoStacksLongFlight : public BasicTwoStacks<aloe::tcp::NoCongestionControl> {
    protected:
        TcpTwoStacksLongFlight()
            : BasicTwoStacks({}, {.send_segments = 128}) {
        }
    };

    /// The server holds four packets per connection: small segments reach that cap long before the byte window closes.
    class TcpTwoStacksFourSlots : public TcpTwoStacks {
    protected:
        TcpTwoStacksFourSlots()
            : TcpTwoStacks({.receive_segments = 4}) {
        }
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacksNoCongestion,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacksLongFlight,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);
    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacksFourSlots,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

}  // namespace

TEST_P(TcpTwoStacks, EchoAcrossMssAndClose) {
    EXPECT_TRUE(run_echo(message, 200)) << "both sides closed within 200 ticks and the echo matched";
    const auto& sc = server_.tcp.counters();
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(sc.connections_accepted, 1U);
    EXPECT_EQ(cc.connections_opened, 1U);
    EXPECT_EQ(sc.connections_closed, 1U);
    EXPECT_EQ(cc.connections_closed, 1U);
    EXPECT_EQ(sc.dropped_no_connection, 0U);
    EXPECT_EQ(cc.dropped_no_connection, 0U);
    EXPECT_EQ(sc.retransmits, 0U);
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(sc.resets_sent + cc.resets_sent, 0U);
    EXPECT_GE(cc.data_segments_sent, 4U) << "5000 bytes is at least four segments at MSS 1460";
    EXPECT_EQ(sc.data_segments_sent, cc.data_segments_sent) << "one echo segment per received segment";
    EXPECT_EQ(sc.dropped_bad_checksum + cc.dropped_bad_checksum, 0U);
    EXPECT_EQ(sc.dropped_out_of_window + cc.dropped_out_of_window, 0U);
    EXPECT_EQ(sc.dropped_no_slot + sc.dropped_no_node, 0U);
    EXPECT_EQ(server_.tcp.table_size() + client_.tcp.table_size(), 0U);
    EXPECT_EQ(server_.wheel.pending() + client_.wheel.pending(), 0U);
}

// The receive slots between two Aloe peers: within them a loss-free fabric means no loss; a segment beyond them is
// dropped by the receiver, and the sender resends it once the slots are free again, by its tail probe and RACK.

TEST_P(TcpTwoStacksFourSlots, SmallSegmentsWithinTheSlotsLoseNothing) {
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(server_.tcp.byte_budget(), 2U * 1460U) << "the byte window offers half of four slots";
    EXPECT_EQ(s->mss(), 1448U) << "two Aloe stacks use timestamps";
    EXPECT_EQ(s->receive_budget(), 4U * 1448U / 2U) << "the connection's budget counts its own full segments";
    EXPECT_EQ(s->window_shift(), 0U) << "four slots' largest budget fits the field unscaled";
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(c->send(aloe::frames::pattern(1)), 1U) << "one byte per segment: send never coalesces";
    }
    exchange();
    EXPECT_EQ(s->unread().size(), 4U) << "four segments, four slots";
    EXPECT_EQ(server_.tcp.counters().dropped_no_slot, 0U);
    EXPECT_EQ(c->acknowledged(), c->committed()) << "everything within the slots is acknowledged";
    s->consume(4);
    exchange();
    EXPECT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    exchange();
    EXPECT_EQ(s->unread().size(), 1U) << "slots freed by consume take segments again";
    EXPECT_EQ(server_.tcp.counters().dropped_no_slot, 0U);

    // Full segments meet the window before the cap. Freeing four bytes moved no edge, since an update waits for a
    // full segment's room: the handshake's edge stands, five bytes past RCV.NXT then, so the window is the budget less
    // five. Two segments fill it, one slot stays free, and nothing is dropped.
    const std::uint32_t window = s->receive_window();
    EXPECT_EQ(window, s->receive_budget() - 5U);
    EXPECT_EQ(c->send(aloe::frames::pattern(4U * 1448U)), window) << "the window stops the sender, not the cap";
    EXPECT_EQ(c->writable(), 0U);
    exchange();
    EXPECT_EQ(s->unread().size(), 1U + window) << "two segments taken, three of four slots used";
    EXPECT_EQ(s->receive_window(), 0U);
    EXPECT_EQ(c->acknowledged(), c->committed());
    EXPECT_EQ(c->writable(), 0U) << "the closed window, not the cap, keeps the client waiting";
    EXPECT_EQ(server_.tcp.counters().dropped_no_slot + server_.tcp.counters().dropped_out_of_window, 0U);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacksFourSlots, SmallSegmentsBeyondTheSlotsRecoverByTheTailProbe) {
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    }
    exchange();
    EXPECT_EQ(s->unread().size(), 4U);
    EXPECT_EQ(server_.tcp.counters().dropped_no_slot, 1U) << "the fifth found no slot";
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_order, 1U) << "the sixth follows a hole";
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_window, 0U)
        << "six bytes sit far inside the 2896-byte window: short segments meet the cap first";
    EXPECT_EQ(s->out_of_order_segments(), 0U) << "with every slot taken in order, nothing is held above the hole";
    s->consume(4);
    // The ACK of the four left the fifth and sixth in flight with nothing reported: the tail probe resends the sixth
    // two round trips later, the receiver, its slots free again, holds it above the hole and reports it, and RACK
    // resends the fifth. The RTO, 200 ms on, is never needed.
    for (int tick = 0; tick < 300 && s->unread().size() < 2; ++tick) {
        exchange();
    }
    EXPECT_EQ(s->unread().size(), 2U) << "the dropped bytes arrived by retransmission";
    EXPECT_EQ(client_.tcp.counters().tail_probes, 1U);
    EXPECT_EQ(client_.tcp.counters().rack_marked, 1U);
    EXPECT_EQ(client_.tcp.counters().fast_retransmits, 1U);
    EXPECT_EQ(client_.tcp.counters().retransmits, 0U);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, ALostOrderIsResentAndTheEchoCompletes) {
    client_port_.set_impairment(
        {.script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
            return egress.index == 3 ? std::optional{aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop}}
                                     : std::nullopt;
        }});
    EXPECT_TRUE(run_echo(5000, 2000));
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_GE(client_.tcp.counters().retransmits + client_.tcp.counters().fast_retransmits, 1U);
}

TEST_P(TcpTwoStacks, ALostEchoSegmentIsResentByTheServer) {
    server_port_.set_impairment(
        {.script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
            return egress.index == 4 ? std::optional{aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop}}
                                     : std::nullopt;
        }});
    EXPECT_TRUE(run_echo(5000, 2000));
    EXPECT_GE(server_.tcp.counters().retransmits + server_.tcp.counters().fast_retransmits, 1U);
}

TEST_P(TcpTwoStacks, RandomLossBothWaysStillCompletesTheEcho) {
    client_port_.set_impairment(
        {.loss = 0.1, .seed = 7});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    server_port_.set_impairment({.loss = 0.1, .seed = 11});  // NOLINT(cert-msc51-cpp)
    EXPECT_TRUE(run_echo(20000, 20000)) << "20 s of virtual time covers the backoffs";
    EXPECT_EQ(client_.tcp.counters().dropped_bad_checksum + server_.tcp.counters().dropped_bad_checksum, 0U);
    EXPECT_GT(client_port_.impairment_counters().dropped, 0U) << "the seed loses some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().dropped, 0U) << "and some of the server's";
    EXPECT_GE(client_.tcp.counters().retransmits + client_.tcp.counters().fast_retransmits, 1U);
    EXPECT_GE(server_.tcp.counters().retransmits + server_.tcp.counters().fast_retransmits, 1U);
}

TEST_P(TcpTwoStacks, DuplicationReorderingAndDelayAreAbsorbed) {
    client_port_.set_impairment({.duplicate = 0.2,
                                 .reorder   = 0.2,
                                 .delay     = 5ms,
                                 .seed      = 3});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    EXPECT_TRUE(run_echo(5000, 4000));
    EXPECT_GE(server_.tcp.counters().dropped_duplicate, 1U) << "the seed duplicates a segment";
    EXPECT_GE(server_.tcp.counters().out_of_order_held, 1U) << "a reordered segment is held, not dropped";
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_order, 0U);
}

TEST_P(TcpTwoStacks, ADelayBeyondTheRtoCausesASpuriousResendThatIsHarmless) {
    // The SYN and the handshake ACK go undelayed, so the RTO stays at 200 ms; everything after is 250 ms late.
    client_port_.set_impairment(
        {.script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
            return egress.index >= 2 ? std::optional{aloe::fabric::Verdict{.action = aloe::fabric::Action::Delay,
                                                                           .delay  = 250ms}}
                                     : std::nullopt;
        }});
    EXPECT_TRUE(run_echo(2000, 4000));
    EXPECT_GE(client_.tcp.counters().retransmits, 1U);
    EXPECT_GE(server_.tcp.counters().dropped_duplicate, 1U);
}

TEST_P(TcpTwoStacksNoCongestion, TheEchoCompletesUnderLossWithoutCongestionControl) {
    client_port_.set_impairment(
        {.loss = 0.1, .seed = 5});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    EXPECT_TRUE(run_echo(20000, 20000));
    EXPECT_GT(client_port_.impairment_counters().dropped, 0U) << "the seed loses some of the client's frames";
    EXPECT_GE(client_.tcp.counters().retransmits + client_.tcp.counters().fast_retransmits, 1U);
}

TEST_P(TcpTwoStacks, ALostSynIsRetransmittedAndTheConnectionEstablishes) {
    client_port_.set_impairment(
        {.script = [](const aloe::fabric::Egress& egress) -> std::optional<aloe::fabric::Verdict> {
            if (egress.index == 0) {
                return aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop};
            }
            return std::nullopt;
        }});
    server_.ip.learn(client_ip, client_mac, now_);
    client_.ip.learn(server_ip, server_mac, now_);
    ASSERT_TRUE(server_.tcp.listen(7).has_value());
    const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
    ASSERT_TRUE(session.has_value());
    Tcp::ConnectionType* client = *session;
    client_.flush(now_);
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1) << "the wire ate the first SYN";

    Tcp::ConnectionType* server = nullptr;
    for (int tick = 0; tick < 15 && client->state() != aloe::tcp::State::Established; ++tick) {
        now_ += 100ms;
        fabric_.advance(now_);
        client_.receive(now_);
        while (client_.tcp.poll_event()) {}
        client_.flush(now_);
        server_.receive(now_);
        while (const auto event = server_.tcp.poll_event()) {
            if (event->events.accepted()) {
                server = event->connection;
            }
        }
        server_.flush(now_);
    }
    EXPECT_EQ(client->state(), aloe::tcp::State::Established);
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(client_.tcp.counters().retransmits, 1) << "the SYN again, one second after the first";
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1) << "only the first SYN was lost";
    client->release();
    server->release();
}

TEST_P(TcpTwoStacks, ADuplicatedSegmentIsCountedAndReadOnce) {
    Tcp::ConnectionType* client = nullptr;
    Tcp::ConnectionType* server = establish(client);
    ASSERT_NE(server, nullptr);
    client_port_.set_impairment({.duplicate = 1.0});
    const auto payload = aloe::frames::pattern(100);
    ASSERT_EQ(client->send(payload), 100U);
    exchange();
    client_port_.set_impairment({});
    EXPECT_EQ(client_port_.impairment_counters().duplicated, 1);
    EXPECT_EQ(server_.tcp.counters().dropped_duplicate, 1);
    EXPECT_EQ(server->unread().size(), 100U) << "read once";
    client->release();
    server->release();
}

TEST_P(TcpTwoStacks, DelayedSegmentsArriveInOrderAfterTheStep) {
    Tcp::ConnectionType* client = nullptr;
    Tcp::ConnectionType* server = establish(client);
    ASSERT_NE(server, nullptr);
    client_port_.set_impairment({.delay = 5ms});
    for (std::uint8_t seed = 0; seed < 3; ++seed) {
        ASSERT_EQ(client->send(aloe::frames::pattern(1000, seed)), 1000U);
    }
    client_.flush(now_);
    EXPECT_EQ(client_port_.impairment_counters().delayed, 3);

    now_ += 1ms;
    fabric_.advance(now_);
    server_.receive(now_);
    EXPECT_EQ(server->unread().size(), 0U) << "nothing before the delay is due";

    now_ += 5ms;
    fabric_.advance(now_);
    server_.receive(now_);
    EXPECT_EQ(server->unread().size(), 3000U);
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_order, 0);
    while (server_.tcp.poll_event()) {}
    server_.flush(now_);
    client_port_.set_impairment({});
    client->release();
    server->release();
}

// The receive side between two Aloe stacks: both offer SACK-permitted, window scaling and timestamps, so the receiver
// holds what arrives above a gap and reports it, and the sender may fill a window past what 16 bits state. The sender
// reads those reports: the cases of its loss recovery between two stacks follow at the end of the file.

TEST_P(TcpTwoStacks, BothOptionsAreNegotiatedBetweenTwoStacks) {
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    for (const Tcp::ConnectionType* end : {c, s}) {
        EXPECT_TRUE(end->sack());
        EXPECT_TRUE(end->window_scaling());
        EXPECT_TRUE(end->timestamps());
        EXPECT_EQ(end->window_shift(), 3U)
            << "chosen for the stack's largest budget, 512 * 1460 / 2 = 373760, counted in segments without timestamps";
        EXPECT_EQ(end->peer_window_shift(), 3U);
        EXPECT_EQ(end->mss(), 1448U) << "1460 less the timestamp option";
        EXPECT_EQ(end->receive_budget(), 370688U) << "the connection's budget counts its own 1448: 512 * 1448 / 2";
    }
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, AReorderedStreamIsHeldAndDeliveredInOrder) {
    client_port_.set_impairment(
        {.reorder = 0.3, .seed = 9});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    EXPECT_TRUE(run_echo(50000, 4000)) << "every byte echoed in order";
    const auto& sc = server_.tcp.counters();
    EXPECT_GT(client_port_.impairment_counters().reordered, 0U) << "the seed reorders some of the client's frames";
    // Holds for this seed: re-check it if the seed or the fabric's reorder model changes.
    EXPECT_GT(sc.out_of_order_held, 0U) << "a data segment that overtook the one before it is held, not dropped";
    EXPECT_EQ(sc.dropped_out_of_order, 0U);
    EXPECT_EQ(sc.dropped_no_slot + sc.dropped_no_node, 0U);
    EXPECT_EQ(client_.tcp.counters().retransmits, 0U) << "a segment held one frame back is never lost";
}

TEST_P(TcpTwoStacks, ALossBehindThreeDuplicateAcksIsRepairedWithoutATimeout) {
    constexpr std::size_t lost = 4;  // the fourth data frame, once: its retransmission is a later frame
    client_port_.set_impairment({.script = [data_frames = std::size_t{0}](const aloe::fabric::Egress& egress) mutable
                                     -> std::optional<aloe::fabric::Verdict> {
        const auto frame = aloe::frames::parse_frame(egress.frame);
        if (!frame || !frame->tcp || aloe::frames::tcp_payload(*frame).empty()) {
            return std::nullopt;
        }
        return ++data_frames == lost ? std::optional{aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop}}
                                     : std::nullopt;
    }});
    // Fourteen full segments, ten behind the loss: the first flight of ten puts six of them behind it. The count held
    // pins that none of the other four left before the fast retransmit, or the server would have held it too.
    EXPECT_TRUE(run_echo(14U * 1448U, 2000));
    const auto& cc = client_.tcp.counters();
    const auto& sc = server_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_EQ(cc.retransmits, 0U) << "no retransmission timeout";
    EXPECT_GE(cc.fast_retransmits, 1U) << "three duplicate ACKs started the repair";
    EXPECT_EQ(sc.out_of_order_held, 6U)
        << "segments 5 to 10, held above the gap: nothing past segment 10 left before the fast retransmit";
    EXPECT_GT(sc.sack_acks_sent, 0U) << "and reported with SACK blocks";
    EXPECT_EQ(sc.dropped_out_of_order, 0U);
}

TEST_P(TcpTwoStacks, ALossWithNothingMoreToSendIsStillRepairedWithoutATimeout) {
    constexpr std::size_t lost = 4;  // the fourth data frame, once: its retransmission is a later frame
    client_port_.set_impairment({.script = [data_frames = std::size_t{0}](const aloe::fabric::Egress& egress) mutable
                                     -> std::optional<aloe::fabric::Verdict> {
        const auto frame = aloe::frames::parse_frame(egress.frame);
        if (!frame || !frame->tcp || aloe::frames::tcp_payload(*frame).empty()) {
            return std::nullopt;
        }
        return ++data_frames == lost ? std::optional{aloe::fabric::Verdict{.action = aloe::fabric::Action::Drop}}
                                     : std::nullopt;
    }});
    // Ten full segments, the whole first flight: segments 1-3 and 5-10 reach the server in one burst and nothing
    // follows them, so the ACKs that burst draws are all the client sees before the repair. Three of them must be
    // true duplicates, although the first answer above the gap also acknowledges segment 3.
    EXPECT_TRUE(run_echo(10U * 1448U, 2000));
    const auto& cc = client_.tcp.counters();
    const auto& sc = server_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_EQ(cc.retransmits, 0U) << "no retransmission timeout";
    EXPECT_GE(cc.fast_retransmits, 1U) << "three duplicate ACKs from one burst started the repair";
    EXPECT_EQ(sc.out_of_order_held, 6U) << "segments 5 to 10, held above the gap";
}

TEST_P(TcpTwoStacks, LossBothWaysStillDeliversEveryByteInOrder) {
    client_port_.set_impairment(
        {.loss = 0.02, .seed = 13});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    server_port_.set_impairment(
        {.loss = 0.02, .seed = 17});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
    EXPECT_TRUE(run_echo(200000, 60000)) << "every byte echoed in order, whatever the counters say";
    EXPECT_GT(client_port_.impairment_counters().dropped, 0U) << "the seed loses some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().dropped, 0U) << "and some of the server's";
}

TEST_P(TcpTwoStacksLongFlight, AWindowPast65535BytesIsUsed) {
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    // The SYN-ACK's field is unscaled, 65535; the first ACK after the handshake states the scaled budget.
    ASSERT_EQ(c->send(aloe::frames::pattern(1)), 1U);
    exchange();
    ASSERT_EQ(c->unacknowledged(), 0U);
    std::size_t past_while_open = 0;  // in flight when the client first passed 65535 with room left to send
    while (c->writable() > 0) {
        ASSERT_GT(c->send(aloe::frames::pattern(c->writable())), 0U);
        if (past_while_open == 0 && c->unacknowledged() > 65535U && c->writable() > 0) {
            past_while_open = c->unacknowledged();
        }
    }
    EXPECT_GT(past_while_open, 65535U) << "more than 16 bits of window in flight, and the client could send more";
    EXPECT_EQ(c->unacknowledged(), 128U * c->mss()) << "the send cap stopped it, not the server's window";
    for (int round = 0; round < 4 && s->unread().size() <= 65535U; ++round) {
        exchange();  // the server takes 64 frames per receive
    }
    EXPECT_GT(s->unread().size(), 65535U) << "the server holds more than 16 bits of window, unread";
    EXPECT_EQ(server_.tcp.counters().dropped_out_of_window + server_.tcp.counters().dropped_no_slot, 0U);
    c->release();
    s->release();
}

// Loss recovery between two Aloe stacks, each egress `one_way` late from before the handshake: the round trip is 10 ms,
// the client's smoothed round trip about 11 ms, so a tail probe waits about 22 ms, and 40 ms more for a lone segment;
// RACK's reordering window is a quarter of the minimum round trip, 2.5 ms; the RTO is srtt plus `rto_min`, over 200 ms.
// Both stacks offer timestamps, so a full segment carries 1448 bytes. The fabric is advanced once per tick.

TEST_P(TcpTwoStacks, TheLastSegmentOfAFlightIsResentByTheProbe) {
    drop_client_data({3});
    // A flight of three at the 11th tick, the third lost. The ACK of the first two, at the 21st, leaves the third alone
    // in flight: its probe goes 2 * 10.875 + 40 ms later, at the 83rd tick, and the echo completes at the 93rd, 82 ms
    // after the drop.
    EXPECT_TRUE(run_echo(3U * 1448U, 2000));
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_GE(cc.tail_probes, 1U);
    EXPECT_EQ(cc.retransmits, 0U) << "no expiry of the RTO";
    ASSERT_TRUE(dropped_at_.has_value());
    ASSERT_TRUE(echoed_at_.has_value());
    EXPECT_LT(*echoed_at_ - *dropped_at_, 100ms) << "well before the RTO";
    EXPECT_GE(client_.tcp.config().rto_min, 200ms) << "the RTO, which never drops below rto_min";
}

TEST_P(TcpTwoStacks, ALoneSegmentLostIsResentByTheProbe) {
    drop_client_data({1});
    // One order of 100 bytes at the 11th tick, lost: its probe goes 2 * 11 + 40 ms later, before the RTO's 211 ms.
    EXPECT_TRUE(run_echo(100, 2000));
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_EQ(cc.tail_probes, 1U);
    EXPECT_EQ(cc.retransmits, 0U);
}

TEST_P(TcpTwoStacks, TwoLoneSegmentsLostInARowAreBothProbed) {
    delay_both_ways();
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    // The first transmission of each of two orders is lost. The first's probe is acknowledged exactly at its end, which
    // leaves its verdict open, and the echo of the probe gives a sample; the second's probe goes all the same, closing
    // that verdict as a loss repaired: Aloe's departure from RFC 8985 7.3 and 7.4.2.
    constexpr std::size_t order = 100;
    const TcpSequence first     = c->committed();
    script_client_segments({first, first + order}, Verdict{.action = Action::Drop});
    for (int sent = 0; sent < 2; ++sent) {
        ASSERT_EQ(c->send(aloe::frames::pattern(order)), order);
        client_.flush(now_);
        for (int t = 0; t < 1000 && c->unacknowledged() > 0; ++t) {
            tick();
        }
        ASSERT_EQ(c->unacknowledged(), 0U) << "order " << sent;
    }
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 2U);
    EXPECT_EQ(cc.tail_probes, 2U);
    EXPECT_EQ(cc.tail_probes_early, 1U);
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(served_.size(), 2U * order);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, AFrameHeldBehindTheNextCausesNoResend) {
    delay_both_ways();
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    // The fourth of a flight of five is one tick later than its flight, so it arrives behind the fifth, the next. The
    // fabric's own `Reorder` would not do on this path: it holds a frame until the next advance and delivers it then,
    // undelayed, four milliseconds ahead of the flight it belongs to. The report of the fifth leaves the fourth RACK's
    // window to arrive in, a quarter of the 10 ms minimum round trip; its ACK comes 1 ms after the report, fills the
    // hole below the highest sequence reported, and is reordering seen.
    const TcpSequence first = c->committed();
    const std::size_t mss   = c->mss();
    const auto flight       = aloe::frames::pattern(5U * mss);
    script_client_segments({first + static_cast<std::uint32_t>(3U * mss)},
                           Verdict{.action = Action::Delay, .delay = one_way + 1ms});
    ASSERT_EQ(c->send(flight), flight.size());
    client_.flush(now_);
    for (int t = 0; t < 1000 && c->unacknowledged() > 0; ++t) {
        tick();
    }
    ASSERT_EQ(c->unacknowledged(), 0U);
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(cc.fast_retransmits, 0U);
    EXPECT_EQ(cc.tail_probes, 0U);
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(cc.reordering_seen, 1U);
    EXPECT_EQ(served_, flight);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, AFrameHeldBehindFourIsResentOnceAndNotTheSecondTime) {
    delay_both_ways();
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    // Twice a flight of six whose second segment is 3 ms later than the rest, behind the four after it, and one more
    // segment half a round trip after the flight. The first time reordering was never seen: once three segments are
    // reported the window is zero, the second is marked lost and resent, and recovery begins with the seventh out. The
    // late original arrived before the resend left, and the ACK of it, which echoes the original's timestamp and leaves
    // the seventh unacknowledged, is a partial ACK in recovery that answers an original: reordering is recorded, and
    // with the resend acknowledged by it the reduction is undone (Linux's `tcp_try_undo_partial`). The resend arrives
    // as a duplicate, and its report, reordering seen, doubles the window to twice a quarter of the 10 ms minimum round
    // trip, 5 ms.
    //
    // The second time, reordering seen is what spares the late segment: three segments reported no longer make the
    // window zero, so RACK waits for it, and its ACK comes 3 ms after the report. The doubled window leaves that wait
    // 2 ms to spare. At the window's base, 2.5 ms, the timer would be due 12.5 ms after the flight, which the wheel
    // rounds up to the tick the ACK arrives in, and the ACK would cancel it only because a tick receives before it
    // advances the wheel.
    //
    // The seventh is what makes that ACK partial. Without it the ACK of the late original covers all that was sent and
    // ends recovery, where the undo records no reordering, as Linux's `tcp_try_undo_recovery` records none and RFC
    // 8985 counts no resent segment as reordered, and the second flight is resent too.
    const TcpSequence first = c->committed();
    const std::size_t mss   = c->mss();
    script_client_segments({first + static_cast<std::uint32_t>(mss), first + static_cast<std::uint32_t>(8U * mss)},
                           Verdict{.action = Action::Delay, .delay = one_way + 3ms});
    const auto& cc = client_.tcp.counters();
    for (int flight = 0; flight < 2; ++flight) {
        ASSERT_EQ(c->send(aloe::frames::pattern(6U * mss)), 6U * mss);
        client_.flush(now_);
        for (int t = 0; t < 5; ++t) {
            tick();
        }
        ASSERT_EQ(c->send(aloe::frames::pattern(mss)), mss);
        client_.flush(now_);
        for (int t = 0; t < 25; ++t) {  // the resend's duplicate report is back two round trips after the flight
            tick();
        }
        ASSERT_EQ(c->unacknowledged(), 0U) << "flight " << flight;
        EXPECT_EQ(cc.fast_retransmits, 1U) << "flight " << flight;
        EXPECT_EQ(cc.undos_original_ack + cc.undos_duplicate_sack, 1U) << "flight " << flight;
        if (flight == 0) {
            EXPECT_GE(cc.reordering_seen, 1U) << "the partial ACK of the late original";
            EXPECT_EQ(c->reordering_window(), 5ms) << "doubled by the resend's duplicate report";
        }
    }
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(served_.size(), 14U * mss);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, NewDataFlowsDuringRecovery) {
    delay_both_ways();
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    // Twenty full segments, written as `writable()` allows; the first transmission of the fourth is lost. Three
    // segments reported behind it mark it lost and begin recovery; PRR lets new data out with its resend.
    const std::size_t mss   = c->mss();
    const auto stream       = aloe::frames::pattern(20U * mss);
    const TcpSequence first = c->committed();
    script_client_segments({first + static_cast<std::uint32_t>(3U * mss)}, Verdict{.action = Action::Drop});
    std::size_t offered = 0;
    std::optional<TcpSequence> at_first_resend;
    std::optional<TcpSequence> at_recovery_end;
    for (int t = 0; t < 1000 && (offered < stream.size() || c->unacknowledged() > 0); ++t) {
        tick([&] {
            // What was committed as the first resend left, and as the ACK that ended recovery arrived: read after the
            // receive that did either, before this tick's writes.
            if (!at_first_resend && client_.tcp.counters().fast_retransmits > 0) {
                at_first_resend = c->committed();
                ASSERT_EQ(c->recovery_state(), RecoveryState::Recovery);
            } else if (at_first_resend && !at_recovery_end && c->recovery_state() == RecoveryState::Open) {
                at_recovery_end = c->committed();
            }
            while (offered < stream.size() && c->writable() > 0) {
                const std::size_t accepted = c->send(std::span<const std::byte>{stream}.subspan(
                    offered, std::min(c->writable(), stream.size() - offered)));
                if (accepted == 0) {
                    break;
                }
                offered += accepted;
            }
        });
    }
    ASSERT_EQ(c->unacknowledged(), 0U);
    ASSERT_TRUE(at_first_resend.has_value());
    ASSERT_TRUE(at_recovery_end.has_value());
    EXPECT_TRUE(at_recovery_end->after(*at_first_resend)) << "new data committed while in recovery";
    const auto& cc = client_.tcp.counters();
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_EQ(cc.recoveries, 1U);
    EXPECT_EQ(cc.retransmits, 0U);
    EXPECT_EQ(served_, stream);
    c->release();
    s->release();
}

TEST_P(TcpTwoStacks, ATransferUnderLossAndReorderingDeliversEveryByte) {
    lose_and_reorder_both_ways();
    EXPECT_TRUE(run_echo(200000, 60000)) << "every byte echoed in order, whatever the counters say";
    EXPECT_GT(client_port_.impairment_counters().dropped, 0U) << "the seed loses some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().dropped, 0U) << "and some of the server's";
    EXPECT_GT(client_port_.impairment_counters().reordered, 0U) << "the seed reorders some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().reordered, 0U) << "and some of the server's";
}

TEST_P(TcpTwoStacksNoCongestion, ATransferUnderLossAndReorderingDeliversEveryByte) {
    lose_and_reorder_both_ways();
    EXPECT_TRUE(run_echo(200000, 60000)) << "every byte echoed in order, whatever the counters say";
    EXPECT_GT(client_port_.impairment_counters().dropped, 0U) << "the seed loses some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().dropped, 0U) << "and some of the server's";
    EXPECT_GT(client_port_.impairment_counters().reordered, 0U) << "the seed reorders some of the client's frames";
    EXPECT_GT(server_port_.impairment_counters().reordered, 0U) << "and some of the server's";
}

TEST_P(TcpTwoStacksFourSlots, AReceiverThatReleasesForRoomDoesNotStallTheSender) {
    drop_client_data({1});
    // Six sends of 100 bytes at the 11th tick, the first lost. At 16 the server holds the second to the fifth in its
    // four slots, refuses the sixth, and reports what it holds; at 21 the report, four segments, marks the first lost
    // and it is resent; at 26 the resend finds every slot taken by data above it, and the server releases the fifth,
    // which it had reported, to take the first. At 31 the ACK of the first four leaves the fifth at the head, reported
    // and gone: the short timer of reneging, max(srtt / 2, 10 ms) with srtt 10.875 ms, clears every mark at 41 and
    // resends the fifth, the ACK of the fifth lets the sixth out at 51, and the echo completes at 61, 50 ms after the
    // drop. Without the short timer the fifth would wait for the RTO, 210.875 ms from the ACK at 31, and the echo would
    // complete at 262, 251 ms after the drop. The client's `sack_reneging` may count the reneging.
    EXPECT_TRUE(run_echo(600, 2000, 100));
    EXPECT_EQ(client_port_.impairment_counters().dropped, 1U);
    EXPECT_GT(server_.tcp.counters().out_of_order_released, 0U);
    ASSERT_TRUE(dropped_at_.has_value());
    ASSERT_TRUE(echoed_at_.has_value());
    EXPECT_LT(*echoed_at_ - *dropped_at_, 100ms) << "by the short timer, not the RTO";
}
