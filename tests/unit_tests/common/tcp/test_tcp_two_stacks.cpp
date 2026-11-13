#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/tcp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
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

    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 10};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 11};
    constexpr std::size_t message = 5000;  ///< More than three segments at MSS 1460.

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

        aloe::loop::ShardCounters counters;
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

        explicit BasicTwoStacks(const Config& server_config = {})
            : server_{server_port_, server_ip, server_config} {
        }

        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_port_ =
            fabric_.add_port({.mac = server_mac, .pool_size = 256, .offloads = GetParam()});
        aloe::fabric::Port& client_port_ =
            fabric_.add_port({.mac = client_mac, .pool_size = 256, .offloads = GetParam()});
        BasicBrick<CongestionControl> server_;
        BasicBrick<CongestionControl> client_{client_port_, client_ip, {}};
        TimePoint now_{};

        /// Learns both MACs, listens, connects, and ticks until both ends are Established. Null on failure.
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
            for (int tick = 0; tick < 10 && (server == nullptr || client->state() != aloe::tcp::State::Established);
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

        /// Connects, sends `bytes`, has the server echo them back and both sides close, within `ticks` of 1 ms.
        /// Returns whether both closed and the echo matched.
        [[nodiscard]] bool run_echo(const std::size_t bytes, const int ticks) {
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
                        offered += c.send(std::span<const std::byte>{sent}.subspan(offered));
                    }
                    if (event->events.readable()) {
                        for (const std::span<const std::byte> chunk : c.unread()) {
                            echoed.insert(echoed.end(), chunk.begin(), chunk.end());
                        }
                        c.consume(c.unread().size());
                        if (echoed.size() == bytes) {
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
// dropped by the receiver, and the sender's retransmission timer resends it once the slots are free again.

TEST_P(TcpTwoStacksFourSlots, SmallSegmentsWithinTheSlotsLoseNothing) {
    Tcp::ConnectionType* c = nullptr;
    Tcp::ConnectionType* s = establish(c);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(server_.tcp.byte_budget(), 2U * 1460U) << "the byte window offers half of four slots";
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
    c->release();
    s->release();
}

TEST_P(TcpTwoStacksFourSlots, SmallSegmentsBeyondTheSlotsRecoverAfterOneRto) {
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
    s->consume(4);
    for (int tick = 0; tick < 300 && s->unread().size() < 2; ++tick) {  // one RTO of 200 ms, then the resend
        exchange();
    }
    EXPECT_EQ(s->unread().size(), 2U) << "the dropped bytes arrived by retransmission";
    EXPECT_GE(client_.tcp.counters().retransmits, 1U);
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
    EXPECT_GE(server_.tcp.counters().dropped_duplicate + server_.tcp.counters().dropped_out_of_order, 1U);
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
