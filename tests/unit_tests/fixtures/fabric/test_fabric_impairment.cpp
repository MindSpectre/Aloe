#include <algorithm>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::fabric::Action;
    using aloe::fabric::Egress;
    using aloe::fabric::Impairment;
    using aloe::fabric::Packet;
    using aloe::fabric::Verdict;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};

    /// A fabric with a source, a sink, and a way to send numbered frames and read the numbers back.
    struct Rig {
        aloe::fabric::Fabric fabric;
        aloe::fabric::Port& source = fabric.add_port({.mac = alpha, .pool_size = 8, .queue_depth = 1024});
        aloe::fabric::Port& sink   = fabric.add_port({.mac = beta, .pool_size = 8, .queue_depth = 1024});

        void send(const std::uint8_t number) {
            auto packet = source.allocate(0);
            ASSERT_TRUE(packet.has_value());
            ASSERT_TRUE(aloe::frames::fill(
                *packet,
                aloe::frames::ethernet_frame(
                    beta, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(1, number))));
            std::array<Packet, 1> burst{std::move(*packet)};
            ASSERT_EQ(source.transmit(0, burst), 1);
        }

        void send_many(const std::uint8_t count) {
            for (std::uint8_t number = 0; number < count; ++number) {
                send(number);
            }
        }

        /// The frame numbers the sink holds, in arrival order.
        std::vector<std::uint8_t> received() {
            std::vector<std::uint8_t> numbers;
            std::array<Packet, 16> burst;
            for (std::size_t count = sink.receive(0, burst); count > 0; count = sink.receive(0, burst)) {
                for (std::size_t index = 0; index < count; ++index) {
                    numbers.push_back(std::to_integer<std::uint8_t>(burst[index].data().back()));
                    burst[index] = Packet{};
                }
            }
            return numbers;
        }
    };

    struct Link : Rig, ::testing::Test {};

    /// The verdict for one egress index and nothing for the rest.
    Impairment only(const std::uint64_t index, const Verdict verdict) {
        Impairment impairment;
        impairment.script = [index, verdict](const Egress& egress) -> std::optional<Verdict> {
            if (egress.index == index) {
                return verdict;
            }
            return std::nullopt;
        };
        return impairment;
    }

}  // namespace

TEST_F(Link, NoImpairmentChangesNothing) {
    source.set_impairment(Impairment{});
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 1, 2}));
    EXPECT_EQ(source.impairment_counters(), aloe::fabric::ImpairmentCounters{});
    EXPECT_EQ(source.counters(0).transmitted, 3);
}

TEST_F(Link, LossDropsSomeFramesAndCountsThem) {
    source.set_impairment({.loss = 0.5, .seed = 7});
    send_many(100);
    const auto got = received();
    EXPECT_GT(got.size(), 20U);
    EXPECT_LT(got.size(), 80U);
    EXPECT_EQ(source.impairment_counters().dropped, 100 - got.size());
    EXPECT_EQ(source.counters(0).transmitted, 100) << "a dropped frame still left the port";
    EXPECT_TRUE(std::ranges::is_sorted(got)) << "loss keeps the order of what survives";
}

TEST_F(Link, TheSameSeedDropsTheSameFrames) {
    source.set_impairment({.loss = 0.3, .seed = 11});
    send_many(50);
    const auto first = received();
    Rig second;
    second.source.set_impairment({.loss = 0.3, .seed = 11});
    second.send_many(50);
    EXPECT_EQ(second.received(), first);
    Rig third;
    third.source.set_impairment({.loss = 0.3, .seed = 12});
    third.send_many(50);
    EXPECT_NE(third.received(), first) << "another seed, another run";
}

TEST_F(Link, DuplicateDeliversTwiceInARow) {
    source.set_impairment({.duplicate = 1.0});
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 0, 1, 1}));
    EXPECT_EQ(source.impairment_counters().duplicated, 2);
}

TEST_F(Link, AReorderedFrameFollowsTheNextOne) {
    source.set_impairment(only(0, Verdict{.action = Action::Reorder}));
    send(0);
    EXPECT_TRUE(received().empty()) << "held";
    send(1);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1, 0}));
    EXPECT_EQ(source.impairment_counters().reordered, 1);
}

TEST_F(Link, ADuplicateStaysTogetherAheadOfAHeldFrame) {
    Impairment impairment;
    impairment.script = [](const Egress& egress) -> std::optional<Verdict> {
        if (egress.index == 0) {
            return Verdict{.action = Action::Reorder};
        }
        if (egress.index == 1) {
            return Verdict{.action = Action::Duplicate};
        }
        return std::nullopt;
    };
    source.set_impairment(impairment);
    send(0);
    send(1);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1, 1, 0}));
    EXPECT_EQ(source.impairment_counters().reordered, 1);
    EXPECT_EQ(source.impairment_counters().duplicated, 1);
}

TEST_F(Link, AReorderHoldIsReleasedByAdvanceWhenNothingFollows) {
    source.set_impairment(only(0, Verdict{.action = Action::Reorder}));
    send(0);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}

TEST_F(Link, ASecondHoldDeliversTheFirst) {
    source.set_impairment({.reorder = 1.0});
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0, 1})) << "each new hold lets the previous one go";
    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{2}));
    EXPECT_EQ(source.impairment_counters().reordered, 3);
}

TEST_F(Link, DelayDeliversAtAdvanceAndNotBefore) {
    fabric.advance(TimePoint{} + 100ms);
    source.set_impairment({.delay = 10ms});
    send(0);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 105ms);
    EXPECT_TRUE(received().empty()) << "due at 110 ms";
    fabric.advance(TimePoint{} + 110ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
    EXPECT_EQ(source.impairment_counters().delayed, 1);
}

TEST_F(Link, EqualDelaysKeepTheOrder) {
    source.set_impairment({.delay = 10ms});
    send_many(20);
    fabric.advance(TimePoint{} + 10ms);
    const auto got = received();
    EXPECT_EQ(got.size(), 20U);
    EXPECT_TRUE(std::ranges::is_sorted(got));
}

TEST_F(Link, JitterStaysWithinItsWindow) {
    source.set_impairment({.delay = 10ms, .jitter = 5ms, .seed = 3});
    send_many(20);
    fabric.advance(TimePoint{} + 9ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 15ms);
    const auto got = received();
    EXPECT_EQ(got.size(), 20U);
    EXPECT_FALSE(std::ranges::is_sorted(got)) << "jitter reorders";
    EXPECT_EQ(std::set<std::uint8_t>(got.begin(), got.end()).size(), 20U);
}

TEST_F(Link, TheScriptOverridesTheModel) {
    Impairment impairment = only(1, Verdict{});
    impairment.loss       = 1.0;
    source.set_impairment(impairment);
    send_many(3);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(source.impairment_counters().dropped, 2);
}

TEST_F(Link, AScriptedDelayTakesItsOwnDuration) {
    source.set_impairment(only(0, Verdict{.action = Action::Delay, .delay = 20ms}));
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    fabric.advance(TimePoint{} + 10ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 20ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}

TEST_F(Link, AScriptedDropIsCounted) {
    source.set_impairment(only(0, Verdict{.action = Action::Drop}));
    send_many(2);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(source.impairment_counters().dropped, 1);
}

TEST_F(Link, CountersAgreeWithTheReceiver) {
    source.set_impairment({.loss = 0.3, .duplicate = 0.3, .seed = 5});
    send_many(200);
    const auto got                           = received();
    const aloe::fabric::ImpairmentCounters c = source.impairment_counters();
    EXPECT_EQ(got.size(), 200 - c.dropped + c.duplicated);
    EXPECT_GT(c.dropped, 0U);
    EXPECT_GT(c.duplicated, 0U);
}

TEST_F(Link, AdvanceWithAnEarlierStampChangesNothing) {
    fabric.advance(TimePoint{} + 10ms);
    source.set_impairment({.delay = 5ms});
    send(0);
    fabric.advance(TimePoint{} + 3ms);
    EXPECT_EQ(fabric.now(), TimePoint{} + 10ms);
    EXPECT_TRUE(received().empty());
    fabric.advance(TimePoint{} + 15ms);
    EXPECT_EQ(received(), (std::vector<std::uint8_t>{0}));
}
