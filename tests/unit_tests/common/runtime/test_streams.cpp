#include <aloe/execution>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/loop>
#include <aloe/net>
#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <net_fixture.hpp>
#include <tcp_fixture.hpp>
#include <tcp_test_device.hpp>

// The senders over a real brick, with the context driven by hand: IP and TCP process, wake, run_once, wake, flush.
// No TcpStack yet; that composition arrives in Task 12.
namespace {

    using namespace std::chrono_literals;
    namespace ex = aloe::execution::ex;
    using aloe::stream::Error;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;
    using Device    = aloe::testing::TcpTestDevice;
    using Ipv4      = aloe::net::Ipv4<Device>;
    using Tcp       = aloe::tcp::Stack<Ipv4>;
    using Streams   = aloe::runtime::Streams<Tcp>;
    using Stream    = aloe::runtime::Stream<Tcp>;
    using TimePoint = aloe::core::TimePoint;
    using Bytes     = std::expected<std::size_t, Error>;  // an alias keeps the comma out of the gtest macros
    using Done      = std::expected<void, Error>;
    using Opened    = std::expected<Stream, Error>;

    constexpr TimePoint start{};

    /// Records one completion; the token lets a test stop the operation from the receiver's side.
    template <typename T>
    struct Recorder {
        using receiver_concept = ex::receiver_t;

        struct Env {
            ex::inplace_stop_token token;
            [[nodiscard]] ex::inplace_stop_token query(ex::get_stop_token_t) const noexcept {
                return token;
            }
        };

        std::optional<T>* value = nullptr;
        int* completions        = nullptr;
        bool* stopped           = nullptr;
        ex::inplace_stop_token token{};

        void set_value(T v) const noexcept {
            *value = std::move(v);
            ++*completions;
        }
        void set_stopped() const noexcept {
            *stopped = true;
            ++*completions;
        }
        [[nodiscard]] Env get_env() const noexcept {
            return Env{token};
        }
    };

    template <typename T>
    struct Slot {
        std::optional<T> value;
        int completions = 0;
        bool stopped    = false;
        ex::inplace_stop_source source;

        [[nodiscard]] Recorder<T> receiver() noexcept {
            return Recorder<T>{&value, &completions, &stopped, source.get_token()};
        }
    };

    class StreamsTest : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& harness_ =
            fabric_.add_port({.mac = aloe::testing::harness_mac, .pool_size = 256, .queue_depth = 4096});
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = aloe::testing::stack_mac, .pool_size = 128});
        Device device_{port_};
        aloe::runtime::ShardContext context_{{.index = 0}, start};
        aloe::runtime::ShardContext::Current current_{context_};
        aloe::loop::ShardQueue<Device> queue_{device_, 0, 1, context_.counters()};
        Ipv4 ip_{queue_, aloe::testing::stack_config()};
        Tcp tcp_{ip_, context_.timers(), {}};
        Streams streams_{tcp_, context_};
        TimePoint now_                           = start;
        std::vector<aloe::fabric::Packet> burst_ = std::vector<aloe::fabric::Packet>(64);
        aloe::frames::TcpPeer peer_{aloe::testing::peer_spec(), TcpSequence{aloe::testing::tcp_peer_isn}};

        StreamsTest() {
            ip_.learn(aloe::testing::harness_ip, aloe::testing::harness_mac, now_);
        }

        /// The receive half of a step: inject nothing or `frame`, process IP and TCP, then the first wake.
        void receive(const std::span<const std::byte> frame) {
            auto packet = harness_.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(*packet, frame));
            std::array<aloe::fabric::Packet, 1> out{std::move(*packet)};
            ASSERT_EQ(harness_.transmit(0, out), 1U);
            const std::size_t received = queue_.receive(burst_);
            ip_.process(std::span<aloe::fabric::Packet>{burst_}.first(received), now_);
            tcp_.process(ip_.received(aloe::wire::Ipv4Protocol::Tcp), now_);
            streams_.wake(now_);
        }

        /// The rest of a step: tasks, the second wake, flush. Returns how many completions ran.
        std::size_t run_step() {
            now_ += 1ms;
            context_.set_now(now_);
            tcp_.process({}, now_);
            streams_.wake(now_);
            std::ignore                = queue_.flush();
            const std::uint64_t before = context_.counters().work_run;
            std::ignore                = context_.run_once(now_);
            streams_.wake(now_);
            tcp_.flush(now_);
            std::ignore = queue_.flush();
            return context_.counters().work_run - before;
        }

        [[nodiscard]] std::vector<aloe::frames::ParsedFrame> on_the_wire() {
            std::ignore = queue_.flush();
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

        /// A connection accepted through the senders, owned by the returned stream.
        [[nodiscard]] Stream accept_one(aloe::frames::TcpPeer& peer) {
            Slot<Opened> slot;
            auto operation = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), slot.receiver());
            ex::start(operation);
            receive(peer.syn());
            auto frames = on_the_wire();
            if (frames.size() != 1) {
                ADD_FAILURE() << "expected the SYN-ACK";
                return {};
            }
            peer.see(frames[0]);
            receive(peer.ack());
            std::ignore = run_step();
            if (slot.completions != 1 || !slot.value || !*slot.value) {
                ADD_FAILURE() << "accept did not complete with a stream";
                return {};
            }
            return std::move(**slot.value);
        }

        [[nodiscard]] static std::vector<std::byte> text(const char* s) {
            std::vector<std::byte> out;
            for (; *s != '\0'; ++s) {
                out.push_back(static_cast<std::byte>(*s));
            }
            return out;
        }
    };

}  // namespace

TEST_F(StreamsTest, ImmediateStateBeforeParking) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    const std::uint64_t work_run = context_.counters().work_run;  // the accept's own completion ran already
    receive(peer_.data(text("abc")));
    Slot<Bytes> readable;
    auto operation = ex::connect(stream.readable(1), readable.receiver());
    ex::start(operation);
    EXPECT_EQ(readable.completions, 1) << "the level is met: no parking, no run_once";
    EXPECT_EQ(readable.value, Bytes{3U});
    EXPECT_EQ(context_.counters().work_run, work_run);
}

TEST_F(StreamsTest, EverySenderValueAndError) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    Slot<Bytes> writable;
    Slot<Done> acked;
    auto readable_op = ex::connect(stream.readable(2), readable.receiver());
    ex::start(readable_op);
    EXPECT_EQ(readable.completions, 0) << "parked";
    receive(peer_.data(text("ab")));
    EXPECT_EQ(readable.completions, 0) << "queued, not run: the wake pass never calls a receiver";
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(readable.value, Bytes{2U});
    std::ignore = on_the_wire();  // the ACK of "ab", sent at the step's flush

    const auto bytes = text("hello");
    Slot<Bytes> sent;
    auto send_op = ex::connect(stream.send(bytes), sent.receiver());
    ex::start(send_op);
    EXPECT_EQ(sent.value, Bytes{5U}) << "writable: completes at once";
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    const TcpSequence after = frames[0].tcp->sequence + 5U;
    auto acked_op           = ex::connect(stream.acked(after), acked.receiver());
    ex::start(acked_op);
    EXPECT_EQ(acked.completions, 0);
    peer_.see(frames[0]);
    receive(peer_.ack());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(acked.value.has_value() && acked.value->has_value());

    auto writable_op = ex::connect(stream.writable(1), writable.receiver());
    ex::start(writable_op);
    EXPECT_EQ(writable.value, Bytes{1460U});

    Slot<Done> close;
    auto close_op = ex::connect(stream.close(), close.receiver());
    ex::start(close_op);
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Fin));
    receive(peer_.ack(frames[0]));
    receive(peer_.fin());
    EXPECT_EQ(run_step(), 1U) << "close() completes when the connection closes";
    EXPECT_TRUE(close.value.has_value() && close.value->has_value());
    Slot<Done> closed;
    auto closed_op = ex::connect(stream.closed(), closed.receiver());
    ex::start(closed_op);
    EXPECT_EQ(closed.completions, 1) << "already closed: completes at once";
    EXPECT_TRUE(closed.value.has_value() && closed.value->has_value()) << "a normal close is not an error";
    stream.release();
    EXPECT_EQ(tcp_.table_size(), 0U);

    // closed() parked on its own: one Closed wait per slot, so close() and closed() never park together.
    std::ignore = on_the_wire();  // the ACK of the peer's FIN and the RST of the release
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    Stream second = accept_one(other);
    ASSERT_TRUE(second.valid());
    Slot<Done> parked;
    auto parked_op = ex::connect(second.closed(), parked.receiver());
    ex::start(parked_op);
    EXPECT_EQ(parked.completions, 0) << "parked";
    second.connection().close();  // the brick's synchronous close, reached through connection()
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_TRUE(frames[0].tcp->flags.has(TcpFlag::Fin));
    receive(other.ack(frames[0]));
    receive(other.fin());
    EXPECT_EQ(run_step(), 1U) << "closed() completes";
    EXPECT_TRUE(parked.value.has_value() && parked.value->has_value()) << "a normal close is not an error";
}

TEST_F(StreamsTest, ResetCompletesWaitersWithErrors) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    Slot<Done> closed;
    auto readable_op = ex::connect(stream.readable(1), readable.receiver());
    auto closed_op   = ex::connect(stream.closed(), closed.receiver());
    ex::start(readable_op);
    ex::start(closed_op);
    receive(peer_.rst());
    EXPECT_EQ(run_step(), 2U);
    EXPECT_EQ(readable.value, Bytes{std::unexpected{Error::Reset}});
    EXPECT_EQ(closed.value, Done{std::unexpected{Error::Reset}});
    Slot<Bytes> late;
    auto late_op = ex::connect(stream.writable(1), late.receiver());
    ex::start(late_op);
    EXPECT_EQ(late.value, Bytes{std::unexpected{Error::Reset}}) << "terminal state is remembered";
}

TEST_F(StreamsTest, ConnectValueAndErrors) {
    Slot<Opened> refused;
    Ipv4 bare{
        queue_, {.address = aloe::testing::stack_ip, .prefix = 24}
    };
    Tcp bare_tcp{bare, context_.timers(), {}};
    Streams bare_streams{bare_tcp, context_};
    auto bare_op = ex::connect(bare_streams.connect({.address = aloe::testing::far_ip, .port = 7}), refused.receiver());
    ex::start(bare_op);
    EXPECT_EQ(refused.completions, 1);
    ASSERT_TRUE(refused.value.has_value());
    EXPECT_EQ(refused.value->error(), Error::NoRoute);

    Slot<Opened> connected;
    auto op =
        ex::connect(streams_.connect({.address = aloe::testing::harness_ip, .port = aloe::testing::tcp_peer_port}),
                    connected.receiver());
    ex::start(op);
    EXPECT_EQ(connected.completions, 0) << "parked on Connected";
    auto frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    receive(peer_.syn_ack(frames[0]));
    EXPECT_EQ(run_step(), 1U);
    ASSERT_TRUE(connected.value.has_value() && connected.value->has_value());
    Stream stream = std::move(**connected.value);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Established);
    EXPECT_EQ(on_the_wire().size(), 1U) << "the deferred handshake ACK left at the flush";

    Slot<Opened> reset;
    aloe::frames::TcpPeer other{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    auto reset_op =
        ex::connect(streams_.connect({.address = aloe::testing::harness_ip, .port = 40001}), reset.receiver());
    ex::start(reset_op);
    frames = on_the_wire();
    ASSERT_EQ(frames.size(), 1U);
    other.see(frames[0]);
    receive(other.rst());
    EXPECT_EQ(run_step(), 1U);
    ASSERT_TRUE(reset.value.has_value());
    EXPECT_EQ(reset.value->error(), Error::Refused) << "a reset before Connected is a refusal";
    EXPECT_EQ(tcp_.table_size(), 1U) << "the refused slot was released by the operation";
}

TEST_F(StreamsTest, AcceptedBacklogAndListenerErrors) {
    Slot<Opened> first;
    auto first_op = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), first.receiver());
    ex::start(first_op);  // creates the listener
    std::vector<aloe::frames::TcpPeer> peers;
    for (std::uint16_t port = 40000; port < 40003; ++port) {
        peers.emplace_back(aloe::testing::peer_spec(port), TcpSequence{1000U});
    }
    for (auto& peer : peers) {
        receive(peer.syn());
        auto frames = on_the_wire();
        ASSERT_EQ(frames.size(), 1U);
        peer.see(frames[0]);
        receive(peer.ack());
    }
    EXPECT_EQ(run_step(), 1U) << "the parked accept took the first";
    ASSERT_TRUE(first.value.has_value() && first.value->has_value());
    EXPECT_EQ((**first.value).remote().port, 40000U);
    for (std::uint16_t port = 40001; port < 40003; ++port) {
        Slot<Opened> next;
        auto op = ex::connect(streams_.accept(aloe::testing::tcp_listen_port), next.receiver());
        ex::start(op);
        EXPECT_EQ(next.completions, 1) << "from the backlog, at once";
        ASSERT_TRUE(next.value.has_value() && next.value->has_value());
        EXPECT_EQ((**next.value).remote().port, port) << "in arrival order";
        (**next.value).release();
    }
    Tcp small{ip_, context_.timers(), {.listeners = 1}};
    Streams small_streams{small, context_};
    ASSERT_TRUE(small.listen(80).has_value());
    Slot<Opened> full;
    auto full_op = ex::connect(small_streams.accept(81), full.receiver());
    ex::start(full_op);
    ASSERT_TRUE(full.value.has_value());
    EXPECT_EQ(full.value->error(), Error::TableFull);
    Slot<Opened> in_use;
    auto in_use_op = ex::connect(small_streams.accept(80), in_use.receiver());
    ex::start(in_use_op);
    ASSERT_TRUE(in_use.value.has_value());
    EXPECT_EQ(in_use.value->error(), Error::Refused) << "listened outside Streams: not ours to serve";
}

TEST_F(StreamsTest, PeerClosedBelowThresholdAndUnreadStaysReadable) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(10), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("abc")));
    EXPECT_EQ(run_step(), 0U) << "three is below ten";
    receive(peer_.fin());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(readable.value, Bytes{std::unexpected{Error::PeerClosed}});
    Slot<Bytes> smaller;
    auto smaller_op = ex::connect(stream.readable(3), smaller.receiver());
    ex::start(smaller_op);
    EXPECT_EQ(smaller.value, Bytes{3U}) << "what came before the FIN is readable";
}

TEST_F(StreamsTest, PositiveWritableThreshold) {
    peer_.spec().window = 40;
    Stream stream       = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    EXPECT_EQ(stream.writable(), 40U);
    Slot<Bytes> writable;
    auto op = ex::connect(stream.writable(100), writable.receiver());
    ex::start(op);
    EXPECT_EQ(writable.completions, 0) << "forty is below a hundred: parked";
    peer_.spec().window = 200;
    receive(peer_.ack());
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(writable.value, Bytes{200U});
}

TEST_F(StreamsTest, TwoDrainsQueueOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));  // wake number one queued it
    streams_.wake(now_);             // wake number two finds nothing parked
    EXPECT_EQ(readable.completions, 0);
    EXPECT_EQ(run_step(), 1U) << "one node in the chain";
    EXPECT_EQ(readable.completions, 1);
}

TEST_F(StreamsTest, ReleaseQueuedCompletionThenReuseSlot) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    const std::uint32_t index = stream.index();
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));  // queued with value 1
    stream.release();                // before the completion ran
    std::ignore = on_the_wire();     // the RST
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    Stream reused = accept_one(next);
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(reused.index(), index) << "the same slot";
    EXPECT_EQ(readable.completions, 1) << "the old completion ran during the accept's step";
    EXPECT_TRUE(readable.stopped) << "stopped, with its own saved state; the reused slot was never touched";
    EXPECT_FALSE(reused.connection().events().any());
    Slot<Bytes> fresh;
    auto fresh_op = ex::connect(reused.readable(1), fresh.receiver());
    ex::start(fresh_op);
    EXPECT_EQ(fresh.completions, 0) << "no stale wake reached the new connection";
    reused.release();  // a started operation outlives its completion: drain it before its state goes out of scope
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(fresh.stopped);
}

TEST_F(StreamsTest, CancelQueuedCompletionAndCancelParked) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    receive(peer_.data(text("x")));
    stream.cancel();  // the completion is queued: its outcome becomes stopped, nothing is enqueued twice
    EXPECT_EQ(readable.completions, 0);
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(readable.stopped);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Closed) << "cancel aborts";
    EXPECT_TRUE(on_the_wire().back().tcp->flags.has(TcpFlag::Rst));
    Slot<Bytes> after;
    auto after_op = ex::connect(stream.writable(1), after.receiver());
    ex::start(after_op);
    EXPECT_TRUE(after.stopped) << "started after cancel: stopped at once";
}

TEST_F(StreamsTest, DeadlineStopsAllWaitersAndAbortsOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    Slot<Done> acked;
    auto readable_op = ex::connect(stream.readable(1), readable.receiver());
    auto acked_op    = ex::connect(stream.acked(stream.committed() + 1U), acked.receiver());
    ex::start(readable_op);
    ex::start(acked_op);
    stream.deadline(now_ + 5ms);
    EXPECT_EQ(context_.timers().pending(), 1U) << "one timer per connection";
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(run_step(), 0U);
    }
    EXPECT_EQ(run_step(), 2U) << "the deadline fired inside run_once and both completions ran next";
    EXPECT_TRUE(readable.stopped);
    EXPECT_TRUE(acked.stopped);
    EXPECT_EQ(tcp_.counters().resets_sent, 1U);
}

TEST_F(StreamsTest, ReceiverStopDrainsParkedOperation) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    readable.source.request_stop();  // the scope's stop at shutdown looks like this
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(readable.stopped);
    EXPECT_EQ(stream.state(), aloe::tcp::State::Established) << "a receiver stop does not abort the connection";
    Slot<Bytes> already;
    Recorder<Bytes> receiver = already.receiver();
    already.source.request_stop();
    auto already_op = ex::connect(stream.readable(1), receiver);
    ex::start(already_op);
    EXPECT_TRUE(already.stopped) << "an already requested token completes stopped in start";
}

TEST_F(StreamsTest, ReceiverStopThenReleaseBeforeItRuns) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    Slot<Bytes> readable;
    auto op = ex::connect(stream.readable(1), readable.receiver());
    ex::start(op);
    readable.source.request_stop();  // queued stopped, still on the slot's active list with its slot callback
    stream.release();                // drops that callback before the slot's stop source is rebuilt
    std::ignore = on_the_wire();     // the RST
    aloe::frames::TcpPeer next{aloe::testing::peer_spec(40001), TcpSequence{2000U}};
    Stream reused = accept_one(next);
    ASSERT_TRUE(reused.valid());
    EXPECT_EQ(readable.completions, 1);
    EXPECT_TRUE(readable.stopped);
    Slot<Bytes> fresh;
    auto fresh_op = ex::connect(reused.readable(1), fresh.receiver());
    ex::start(fresh_op);
    reused.cancel();
    EXPECT_EQ(run_step(), 1U);
    EXPECT_TRUE(fresh.stopped) << "the rebuilt stop source reached the new wait";
    EXPECT_EQ(fresh.completions, 1);
}

TEST_F(StreamsTest, MoveOwnerReleasesOnce) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    {
        Stream moved = std::move(stream);
        EXPECT_FALSE(stream.valid());  // NOLINT(bugprone-use-after-move): the moved-from handle is inert by contract
        EXPECT_TRUE(moved.valid());
        EXPECT_EQ(tcp_.table_size(), 1U);
    }
    EXPECT_EQ(tcp_.table_size(), 0U) << "the owner's destructor released";
    stream.release();  // NOLINT(bugprone-use-after-move): inert: a no-op
    EXPECT_EQ(tcp_.counters().resets_sent, 1U);
}

TEST_F(StreamsTest, SendRefusalParksUntilRetryHint) {
    Stream stream = accept_one(peer_);
    ASSERT_TRUE(stream.valid());
    device_.refuse_transmit = true;
    auto filler             = port_.allocate(0);
    ASSERT_TRUE(filler.has_value());
    std::ignore      = queue_.transmit(std::move(*filler));  // the one-slot ring now holds a packet the device refuses
    const auto bytes = text("hello");
    Slot<Bytes> sent;
    auto op = ex::connect(stream.send(bytes), sent.receiver());
    ex::start(op);
    EXPECT_EQ(sent.completions, 0) << "refused: parked, although writable() is still positive";
    EXPECT_EQ(stream.writable(), 1460U);
    EXPECT_EQ(run_step(), 1U) << "the retry hint woke it, and the retry was refused again: parked again";
    EXPECT_EQ(sent.completions, 0);
    device_.refuse_transmit = false;
    EXPECT_EQ(run_step(), 1U);
    EXPECT_EQ(sent.value, Bytes{5U});
    const auto frames = on_the_wire();
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(aloe::frames::tcp_payload(frames.back()).size(), 5U);
}
