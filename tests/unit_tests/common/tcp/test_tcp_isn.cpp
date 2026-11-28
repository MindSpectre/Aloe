#include <aloe/core>
#include <aloe/tcp>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

// IsnGenerator, RFC 6528's numbers as Linux's `secure_tcp_seq_and_ts_off` computes them: one SipHash-2-4 of the flow
// under the generator's key, its low half plus the clock in 64 ns units the initial sequence number, its high half the
// timestamp offset.
namespace {

    using namespace std::chrono_literals;
    using aloe::core::SipHashKey;
    using aloe::core::TimePoint;
    using aloe::tcp::Endpoint;
    using aloe::tcp::detail::IsnGenerator;
    using aloe::wire::Ipv4Address;
    using aloe::wire::TcpSequence;

    constexpr SipHashKey key{.k0 = 0x0706'0504'0302'0100U, .k1 = 0x0f0e'0d0c'0b0a'0908U};
    constexpr TimePoint t0{};

    struct Flow {
        Endpoint local{};
        Endpoint remote{};
    };

    // 10.0.0.2:40000 -> 10.0.0.1:443.
    constexpr Flow flow{
        .local  = {.address = Ipv4Address{10, 0, 0, 2}, .port = 40000},
        .remote = {.address = Ipv4Address{10, 0, 0, 1}, .port = 443  },
    };

    [[nodiscard]] IsnGenerator::Numbers numbers(IsnGenerator& generator, const Flow& of, const TimePoint at) {
        return generator.numbers(of.local, of.remote, at);
    }

}  // namespace

TEST(TcpIsn, TwoFlowsThatDifferInOneFieldGetUnrelatedNumbers) {
    IsnGenerator generator{key};
    const IsnGenerator::Numbers base = numbers(generator, flow, t0);
    // The local address, the remote address, the local port and the remote port, one changed in each.
    std::array<Flow, 4> others{flow, flow, flow, flow};
    others[0].local.address  = Ipv4Address{10, 0, 0, 3};
    others[1].remote.address = Ipv4Address{10, 0, 0, 4};
    others[2].local.port     = 40001;
    others[3].remote.port    = 444;
    for (std::size_t field = 0; field < others.size(); ++field) {
        SCOPED_TRACE(::testing::Message() << "field " << field);
        const IsnGenerator::Numbers changed = numbers(generator, others[field], t0);
        EXPECT_NE(changed.isn, base.isn);
        EXPECT_NE(changed.timestamp_offset, base.timestamp_offset);
    }
}

TEST(TcpIsn, OneFlowLaterStartsFurtherOnByTheClock) {
    IsnGenerator generator{key};
    const std::uint32_t before = numbers(generator, flow, t0).isn.value;
    const std::uint32_t later  = numbers(generator, flow, t0 + 64us).isn.value;
    EXPECT_EQ(static_cast<std::uint32_t>(later - before), 1000U) << "64 us is 1000 units of 64 ns";
}

TEST(TcpIsn, TheClockTermWrapsWithTheSequenceSpace) {
    IsnGenerator generator{key};
    const std::uint32_t before = numbers(generator, flow, t0).isn.value;
    const std::uint32_t later  = numbers(generator, flow, t0 + 275s).isn.value;
    const auto units           = static_cast<std::uint32_t>(std::uint64_t{275'000'000'000} >> 6U);
    EXPECT_EQ(static_cast<std::uint32_t>(later - before), units) << "275 s is past 2^32 units of 64 ns";
    // The nanoseconds are shifted in 64 bits, then truncated. At 275 s the order does not show, since 275e9 >> 32 is
    // 64, a multiple of 64; at 300 s truncating first would give 56988384.
    const std::uint32_t further = numbers(generator, flow, t0 + 300s).isn.value;
    const auto more_units       = static_cast<std::uint32_t>(std::uint64_t{300'000'000'000} >> 6U);
    EXPECT_EQ(more_units, 392532704U);
    EXPECT_EQ(static_cast<std::uint32_t>(further - before), more_units) << "shifted before it is truncated";
}

TEST(TcpIsn, TheOffsetIsTheSameForOneFlowAtAnyStamp) {
    IsnGenerator generator{key};
    EXPECT_EQ(numbers(generator, flow, t0).timestamp_offset, numbers(generator, flow, t0 + 1h).timestamp_offset);
}

TEST(TcpIsn, AnotherKeyGivesUnrelatedNumbers) {
    IsnGenerator generator{key};
    IsnGenerator other{
        SipHashKey{.k0 = key.k0, .k1 = key.k1 + 1}
    };
    const IsnGenerator::Numbers ours   = numbers(generator, flow, t0);
    const IsnGenerator::Numbers theirs = numbers(other, flow, t0);
    EXPECT_NE(ours.isn, theirs.isn);
    EXPECT_NE(ours.timestamp_offset, theirs.timestamp_offset);
}

TEST(TcpIsn, AForcedNumberIsUsedOnceAndLeavesTheOffset) {
    IsnGenerator generator{key};
    const IsnGenerator::Numbers computed = numbers(generator, flow, t0);
    const TcpSequence forced{computed.isn.value + 12345U};
    generator.force(forced);
    const IsnGenerator::Numbers first = numbers(generator, flow, t0);
    EXPECT_EQ(first.isn, forced);
    EXPECT_EQ(first.timestamp_offset, computed.timestamp_offset) << "the offset is the flow's, forced or not";
    const IsnGenerator::Numbers second = numbers(generator, flow, t0);
    EXPECT_EQ(second.isn, computed.isn) << "the force is spent";
    EXPECT_EQ(second.timestamp_offset, computed.timestamp_offset);
}

TEST(TcpIsn, TheNumbersAreTheHashOfTheFlow) {
    // The local address, the remote address, the local port and the remote port, each little-endian.
    constexpr std::array message{
        std::byte{0x02},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x0a},  // 10.0.0.2
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x0a},  // 10.0.0.1
        std::byte{0x40},
        std::byte{0x9c},  // 40000
        std::byte{0xbb},
        std::byte{0x01},  // 443
    };
    const std::uint64_t hash = aloe::core::siphash24(key, std::span<const std::byte>{message});
    IsnGenerator generator{key};
    const IsnGenerator::Numbers at_epoch = numbers(generator, flow, t0);
    EXPECT_EQ(at_epoch.isn.value, static_cast<std::uint32_t>(hash)) << "no clock term at the epoch";
    EXPECT_EQ(at_epoch.timestamp_offset, static_cast<std::uint32_t>(hash >> 32U));
}
