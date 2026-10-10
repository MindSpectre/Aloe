#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
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
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress server_mac{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress client_mac{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::Ipv4Address server_ip{10, 0, 0, 10};
    constexpr aloe::wire::Ipv4Address client_ip{10, 0, 0, 11};
    constexpr std::size_t message = 5000;  ///< More than three segments at MSS 1460.

    /// One brick with its loop state: what a program writes once per shard.
    struct Brick {
        Brick(aloe::fabric::Port& port, const aloe::wire::Ipv4Address address)
            : queue{
                  port, 0, 64, counters
        },
              ip{queue, {.address = address, .prefix = 24}}, tcp{ip, wheel, {}} {
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

    class TcpTwoStacks : public ::testing::TestWithParam<aloe::fabric::EmulatedOffloads> {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& server_port_ =
            fabric_.add_port({.mac = server_mac, .pool_size = 256, .offloads = GetParam()});
        aloe::fabric::Port& client_port_ =
            fabric_.add_port({.mac = client_mac, .pool_size = 256, .offloads = GetParam()});
        Brick server_{server_port_, server_ip};
        Brick client_{client_port_, client_ip};
        TimePoint now_{};
    };

    INSTANTIATE_TEST_SUITE_P(Offloads,
                             TcpTwoStacks,
                             ::testing::Values(aloe::fabric::EmulatedOffloads::None,
                                               aloe::fabric::EmulatedOffloads::Checksums),
                             aloe::testing::offloads_name);

    /// The echo's own copy: unread bytes go back out through prepare and commit, in place on the way out.
    void echo(Tcp::ConnectionType& c) {
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

}  // namespace

TEST_P(TcpTwoStacks, EchoAcrossMssAndClose) {
    server_.ip.learn(client_ip, client_mac, now_);
    client_.ip.learn(server_ip, server_mac, now_);
    ASSERT_TRUE(server_.tcp.listen(7).has_value());
    const auto sent = aloe::frames::pattern(message);
    std::vector<std::byte> echoed;
    std::size_t offered = 0;
    bool server_done    = false;
    bool client_done    = false;

    const auto session = client_.tcp.connect({.address = server_ip, .port = 7}, now_);
    ASSERT_TRUE(session.has_value());
    Tcp::ConnectionType& c = **session;

    for (int tick = 0; tick < 200 && !(server_done && client_done); ++tick) {
        now_ += 1ms;
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
                if (echoed.size() == message) {
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

    EXPECT_TRUE(server_done && client_done) << "both sides closed within 200 ticks";
    EXPECT_EQ(echoed, sent);
    EXPECT_EQ(offered, message);
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
