#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_fixture.hpp>

namespace {

    using namespace std::chrono_literals;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Stack     = aloe::runtime::TcpStack<aloe::fabric::Port>;
    using Shard     = aloe::runtime::Shard<aloe::fabric::Port, Stack>;
    using TimePoint = aloe::core::TimePoint;

    static_assert(aloe::runtime::IsStack<Stack, aloe::fabric::Port>);
    static_assert(aloe::runtime::detail::HasOnTick<Stack> && aloe::runtime::detail::HasOnFlush<Stack>);

    constexpr TimePoint start{};

    /// Stops a shard the test thread plays and steps it until it closes, so no operation outlives its owner.
    void drain(Shard& shard, TimePoint now) {
        {
            const aloe::runtime::ShardContext::Current current{shard.context()};
            shard.context().request_stop();
        }
        for (int step = 0; step < 100 && !shard.context().try_finish(); ++step) {
            now         += std::chrono::milliseconds{1};
            std::ignore  = shard.step(now);
        }
        EXPECT_TRUE(shard.context().drained());
    }

    class TcpRuntime : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& harness_ =
            fabric_.add_port({.mac = aloe::testing::harness_mac, .pool_size = 256, .queue_depth = 4096});
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = aloe::testing::stack_mac, .pool_size = 128});
        aloe::runtime::ShardConfig config_{.transmit_ring = 64, .idle = aloe::runtime::IdlePolicy::Yield};
        Shard shard_{config_, port_, 0, start, aloe::testing::stack_config(), aloe::tcp::TcpConfig{}};
        TimePoint now_ = start;
        aloe::frames::TcpPeer peer_{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};
        std::list<Shard> more_{};  ///< After the ports they use, so destroyed before them.

        TcpRuntime() {
            shard_.stack().ip().learn(aloe::testing::harness_ip, aloe::testing::harness_mac, start);
        }

    public:
        TcpRuntime(const TcpRuntime&)            = delete;
        TcpRuntime& operator=(const TcpRuntime&) = delete;
        TcpRuntime(TcpRuntime&&)                 = delete;
        TcpRuntime& operator=(TcpRuntime&&)      = delete;

        ~TcpRuntime() override {
            for (Shard& shard : more_) {
                drain(shard, now_);
            }
            drain(shard_, now_);
        }

    protected:
        /// Another shard over a port of the fabric, drained and destroyed with the fixture.
        Shard& add_shard(aloe::fabric::Port& port,
                         const std::uint16_t queue,
                         const aloe::net::Ipv4Config& ip,
                         const aloe::tcp::TcpConfig& tcp) {
            return more_.emplace_back(config_, port, queue, start, ip, tcp);
        }

        void inject(const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1U);
        }

        bool step(const aloe::core::Duration by = 1ms) {
            now_ += by;
            return shard_.step(now_);
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> on_the_wire() {
            std::vector<aloe::frames::ParsedFrame> frames;
            std::array<aloe::fabric::Packet, 16> out;
            for (std::size_t count = harness_.receive(0, out); count > 0; count = harness_.receive(0, out)) {
                for (std::size_t index = 0; index < count; ++index) {
                    frames.push_back(aloe::frames::parse_frame(aloe::frames::bytes_of(out[index])).value());
                    out[index] = aloe::fabric::Packet{};
                }
            }
            return frames;
        }

        /// Spawns on the shard thread, which this test thread plays.
        template <typename Task>
        void spawn(Task&& task) {
            const aloe::runtime::ShardContext::Current current{shard_.context()};
            shard_.scheduler().spawn(std::forward<Task>(task));
        }
    };

    /**
     * Echoes one connection, then closes it. Copies into the prepared segment, consumes, then commits: the
     * consume moves the window's edge before the segment is sealed, so the echo carries the ACK and the
     * window it answers and no window update follows it. (Awaiting `send(chunk)` first and consuming after
     * would leave the edge's move for a pure ACK at the flush: `chunk` must outlive the send.)
     */
    aloe::runtime::task<void> echo(Stack::StreamType stream, int* echoed) {
        for (;;) {
            const auto readable = co_await stream.readable(1);
            if (!readable) {
                break;
            }
            while (!stream.unread().empty()) {
                const auto chunk       = stream.unread().front();
                const std::size_t size = std::min(chunk.size(), stream.writable());
                const auto room        = size == 0 ? std::nullopt : stream.prepare(size);
                if (!room) {
                    co_return;
                }
                std::ranges::copy(chunk.first(size), room->begin());
                stream.consume(size);
                if (!stream.commit(size)) {
                    co_return;
                }
                ++*echoed;
            }
        }
        std::ignore = co_await stream.close();
    }

    aloe::runtime::task<void> serve(Stack::StreamsType* streams, aloe::runtime::Scheduler scheduler, int* echoed) {
        for (;;) {
            auto stream = co_await streams->accept(aloe::testing::tcp_listen_port);
            if (!stream) {
                break;
            }
            scheduler.spawn(echo(std::move(*stream), echoed));
        }
    }

    aloe::runtime::task<void> connect_and_record(Stack::StreamsType* streams,
                                                 const aloe::tcp::Endpoint peer,
                                                 std::optional<aloe::stream::Error>* outcome,
                                                 int* completions) {
        auto stream = co_await streams->connect(peer);
        ++*completions;
        if (!stream) {
            *outcome = stream.error();
        }
    }

}  // namespace

TEST_F(TcpRuntime, SameTickReplyPiggybacksAck) {
    int echoed = 0;
    spawn(serve(&shard_.stack().streams(), shard_.scheduler(), &echoed));
    EXPECT_FALSE(step()) << "the task ran inside spawn and parked on accept";
    inject(peer_.syn());
    EXPECT_TRUE(step());
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    peer_.see(frames[0]);
    inject(peer_.ack());
    EXPECT_TRUE(step());
    EXPECT_TRUE(on_the_wire().empty());
    inject(peer_.data(aloe::frames::pattern(10)));
    EXPECT_TRUE(step());
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U) << "the reply, and no pure ACK before or after it";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 10U);
    EXPECT_EQ(frames[0].tcp->acknowledgement, peer_.snd_nxt());
    EXPECT_EQ(shard_.stack().tcp().counters().pure_acks_sent, 0U);
    EXPECT_EQ(echoed, 1);
    EXPECT_EQ(shard_.context().counters().work_run, 2U) << "the accept completion and the readable completion";
}

TEST_F(TcpRuntime, ZeroWindowReopeningIsAdvertisedInTheSameTick) {
    aloe::fabric::Port& tiny_port = fabric_.add_port({
        .mac = {0x02, 0, 0, 0, 0, 0x33},
          .pool_size = 128
    });
    Shard& tiny                   = add_shard(tiny_port,
                                              0,
                                              aloe::net::Ipv4Config{
                                                  .address = {10, 0, 0, 3},
                                                    .prefix = 24
    },
                                              aloe::tcp::TcpConfig{.receive_segments = 1});
    tiny.stack().ip().learn(aloe::testing::harness_ip, aloe::testing::harness_mac, start);
    int echoed = 0;
    {
        const aloe::runtime::ShardContext::Current current{tiny.context()};
        tiny.scheduler().spawn(serve(&tiny.stack().streams(), tiny.scheduler(), &echoed));
    }
    aloe::frames::TcpSpec spec = aloe::testing::peer_spec();
    spec.destination_mac       = {0x02, 0, 0, 0, 0, 0x33};
    spec.destination           = {10, 0, 0, 3};
    aloe::frames::TcpPeer peer{spec, TcpSequence{1000U}};
    std::ignore = tiny.step(now_ += 1ms);
    inject(peer.syn());
    std::ignore = tiny.step(now_ += 1ms);
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    peer.see(frames[0]);
    inject(peer.ack());
    std::ignore = tiny.step(now_ += 1ms);
    inject(peer.data(aloe::frames::pattern(1460)));  // the whole budget
    std::ignore = tiny.step(now_ += 1ms);
    frames      = on_the_wire();
    ASSERT_EQ(frames.size(), 1U) << "the echo of the full segment";
    EXPECT_EQ(frames[0].tcp->window, 1460U) << "consumed by the task before the reply: the reopening rides along";
    EXPECT_EQ(aloe::frames::tcp_payload(frames[0]).size(), 1460U);
    EXPECT_EQ(echoed, 1);
}

TEST_F(TcpRuntime, TimerEventRunsOnNextEmptyStepAndSurvivesAReceive) {
    std::optional<aloe::stream::Error> outcome;
    int completions = 0;
    spawn(connect_and_record(
        &shard_.stack().streams(), {.address = aloe::testing::harness_ip, .port = 40001}, &outcome, &completions));
    std::ignore = step();
    EXPECT_EQ(on_the_wire().size(), 1U) << "the SYN nobody answers";
    for (int second = 1; second < 63; ++second) {
        std::ignore = step(1s);
        EXPECT_EQ(completions, 0);
    }
    std::ignore = step(1s);  // the timeout fires inside run_once; its completion is queued at on_flush
    EXPECT_EQ(completions, 0) << "not yet: queued work runs on the next step";
    inject(aloe::frames::arp_frame(
        aloe::frames::arp_request(aloe::testing::harness_mac, aloe::testing::harness_ip, aloe::testing::stack_ip),
        aloe::wire::MacAddress::broadcast()));
    std::ignore = step();  // a receive pass does not erase it
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(outcome, aloe::stream::Error::TimedOut);
    EXPECT_EQ(shard_.stack().tcp().table_size(), 0U) << "the failed open released its slot";
}

TEST_F(TcpRuntime, ArpForwardedOnceToSiblings) {
    // Two shards over a two-queue port, bound as siblings by hand and ticked from this thread.
    constexpr aloe::wire::MacAddress pair_mac{0x02, 0, 0, 0, 0, 0x44};
    constexpr aloe::wire::Ipv4Address pair_ip{10, 0, 0, 4};
    aloe::fabric::Port& pair = fabric_.add_port({.mac = pair_mac, .queues = 2, .pool_size = 128});
    const aloe::net::Ipv4Config config{.address = pair_ip, .prefix = 24, .gateway = aloe::testing::gateway_ip};
    Shard& first  = add_shard(pair, 0, config, aloe::tcp::TcpConfig{});
    Shard& second = add_shard(pair, 1, config, aloe::tcp::TcpConfig{});
    const std::array<aloe::runtime::ShardContext*, 1> for_first{&second.context()};
    const std::array<aloe::runtime::ShardContext*, 1> for_second{&first.context()};
    first.context().set_siblings(for_first);
    second.context().set_siblings(for_second);
    inject(aloe::frames::arp_frame(
        aloe::frames::arp_reply(aloe::testing::gateway_mac, aloe::testing::gateway_ip, pair_mac, pair_ip),
        pair_mac));  // ARP: queue 0
    std::ignore = first.step(now_ += 1ms);
    EXPECT_EQ(first.stack().ip().counters().resolutions, 1U);
    EXPECT_FALSE(second.context().inbox().empty()) << "one forward posted";
    std::ignore = first.step(now_ += 1ms);  // an empty tick forwards nothing again
    std::ignore = second.step(now_);
    EXPECT_EQ(second.context().counters().inbox_received, 1U) << "forwarded once";
    EXPECT_EQ(second.stack().ip().resolve(aloe::testing::gateway_ip, now_), aloe::testing::gateway_mac)
        << "learned without asking";
    EXPECT_EQ(second.stack().ip().counters().arp_requests_sent, 0U);
    std::ignore = second.step(now_ += 1ms);
    EXPECT_TRUE(first.context().inbox().empty()) << "learn does not echo the resolution back";
    EXPECT_EQ(first.stack().forwards_dropped() + second.stack().forwards_dropped(), 0U);
}
