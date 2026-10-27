#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::fabric::LoanReturn;
    using aloe::fabric::Packet;
    using TimePoint = aloe::core::TimePoint;

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};
    constexpr aloe::wire::MacAddress gamma_mac{0x02, 0, 0, 0, 0, 0x0c};

    aloe::fabric::PortConfig port(const aloe::wire::MacAddress& mac, const LoanReturn loans = LoanReturn::OnStep) {
        return {.mac = mac, .queues = 1, .pool_size = 4, .loans = loans};
    }

    std::vector<std::byte> frame_to(const aloe::wire::MacAddress& destination, const std::uint8_t seed = 0) {
        return aloe::frames::ethernet_frame(
            destination, alpha, aloe::frames::ethertype_experimental, aloe::frames::pattern(30, seed));
    }

    /// Lends a packet holding `frame` and transmits the handle; returns the lender.
    Packet lend_and_transmit(aloe::fabric::Port& source, std::span<const std::byte> frame) {
        auto lender = source.allocate(0);
        EXPECT_TRUE(lender.has_value());
        if (!lender) {
            return Packet{};
        }
        EXPECT_TRUE(aloe::frames::fill(*lender, frame));
        std::array<Packet, 1> burst{lender->lend()};
        EXPECT_EQ(source.transmit(0, burst), 1);
        EXPECT_TRUE(burst[0].empty());
        return std::move(*lender);
    }

    std::vector<std::vector<std::byte>> drain(aloe::fabric::Port& port) {
        std::vector<std::vector<std::byte>> frames;
        std::array<Packet, 8> burst;
        for (std::size_t count = port.receive(0, burst); count > 0; count = port.receive(0, burst)) {
            for (std::size_t index = 0; index < count; ++index) {
                frames.push_back(aloe::frames::bytes_of(burst[index]));
                burst[index] = Packet{};
            }
        }
        return frames;
    }

    /// How many packets the port's queue 0 can hand out right now.
    std::size_t pool_count(aloe::fabric::Port& port) {
        std::vector<Packet> held;
        while (auto packet = port.allocate(0)) {
            held.push_back(std::move(*packet));
        }
        return held.size();
    }

}  // namespace

TEST(FabricLoans, ATransmittedHandleIsHeldUntilTheStepAndReleasedByTheQueuesNextCall) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port(port(alpha));
    auto& b = fabric.add_port(port(beta));
    EXPECT_TRUE(a.capabilities().tx_reclaim);

    const Packet lender = lend_and_transmit(a, frame_to(beta));
    EXPECT_TRUE(lender.lent());
    EXPECT_EQ(a.loans_outstanding(0), 1);
    EXPECT_EQ(drain(b).size(), 1) << "the receiver got its copy at once";
    EXPECT_TRUE(lender.lent()) << "delivery does not return the loan";

    fabric.advance(TimePoint{} + 1ms);
    EXPECT_EQ(fabric.now(), TimePoint{} + 1ms);
    EXPECT_TRUE(lender.lent()) << "advance finishes the loan; the queue's next call releases it";
    std::array<Packet, 1> none;
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, ReclaimReleasesAnOnStepLoanAtOnce) {
    aloe::fabric::Fabric fabric;
    auto& a             = fabric.add_port(port(alpha));
    std::ignore         = fabric.add_port(port(beta));
    const Packet lender = lend_and_transmit(a, frame_to(beta));
    EXPECT_EQ(a.reclaim(0), 1);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(a.reclaim(0), 0);
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, AdvanceFinishesOnlyWhatWasTransmittedBefore) {
    aloe::fabric::Fabric fabric;
    auto& a            = fabric.add_port(port(alpha));
    std::ignore        = fabric.add_port(port(beta));
    const Packet first = lend_and_transmit(a, frame_to(beta, 1));
    fabric.advance(TimePoint{} + 1ms);
    const Packet second = lend_and_transmit(a, frame_to(beta, 2));  // this transmit releases the finished first
    EXPECT_FALSE(first.lent());
    EXPECT_TRUE(second.lent());
    std::array<Packet, 1> none;
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_TRUE(second.lent()) << "not finished yet";
    fabric.advance(TimePoint{} + 2ms);
    EXPECT_EQ(a.receive(0, none), 0);
    EXPECT_FALSE(second.lent());
}

TEST(FabricLoans, AHeldPortReturnsOnlyOnRequest) {
    aloe::fabric::Fabric fabric;
    auto& held          = fabric.add_port(port(gamma_mac, LoanReturn::Held));
    std::ignore         = fabric.add_port(port(beta));
    const Packet lender = lend_and_transmit(held, frame_to(beta));
    fabric.advance(TimePoint{} + 1ms);
    std::array<Packet, 1> none;
    EXPECT_EQ(held.receive(0, none), 0);
    EXPECT_TRUE(lender.lent()) << "a held port never finishes on its own";
    EXPECT_TRUE(held.capabilities().tx_reclaim);
    EXPECT_EQ(held.reclaim(0), 0);
    EXPECT_TRUE(lender.lent());
    held.return_loans(0);
    EXPECT_FALSE(lender.lent());
    EXPECT_EQ(held.loans_outstanding(0), 0);
}

TEST(FabricLoans, ADroppedLenderGivesItsBlockBackAtTheReturn) {
    aloe::fabric::Fabric fabric;
    auto& a     = fabric.add_port(port(alpha));
    std::ignore = fabric.add_port(port(beta));
    { const Packet lender = lend_and_transmit(a, frame_to(beta)); }
    EXPECT_EQ(a.loans_outstanding(0), 1);
    EXPECT_EQ(pool_count(a), 3) << "the orphaned block is still in flight";
    EXPECT_EQ(a.reclaim(0), 1);
    EXPECT_EQ(pool_count(a), 4) << "given back once, at the return";
}

TEST(FabricLoans, AHandleDroppedWithoutTransmitReturnsAtOnce) {
    aloe::fabric::Fabric fabric;
    auto& a     = fabric.add_port(port(alpha));
    auto lender = a.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame_to(beta)));
    {
        const Packet handle = lender->lend();
        EXPECT_TRUE(lender->lent());
    }
    EXPECT_FALSE(lender->lent());
    EXPECT_EQ(a.loans_outstanding(0), 0);
}

TEST(FabricLoans, AnUnlentPacketTransmitsAsBefore) {
    aloe::fabric::Fabric fabric;
    auto& a     = fabric.add_port(port(alpha));
    auto& b     = fabric.add_port(port(beta));
    auto packet = a.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(*packet, frame_to(beta)));
    std::array<Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(a.transmit(0, burst), 1);
    EXPECT_EQ(a.loans_outstanding(0), 0);
    EXPECT_EQ(drain(b).size(), 1);
    EXPECT_EQ(pool_count(a), 4);
}

TEST(FabricLoans, AChecksumFillWritesIntoTheLentBlock) {
    aloe::fabric::Fabric fabric;
    auto& a = fabric.add_port({.mac = alpha, .pool_size = 4, .offloads = aloe::fabric::EmulatedOffloads::Checksums});
    auto& b = fabric.add_port({.mac = beta, .pool_size = 4, .offloads = aloe::fabric::EmulatedOffloads::Checksums});
    const auto frame = aloe::frames::ipv4_frame(
        {
            .destination_mac  = beta,
            .source_mac       = alpha,
            .source           = aloe::wire::Ipv4Address{10, 0, 0, 1},
            .destination      = aloe::wire::Ipv4Address{10, 0, 0, 2},
            .source_port      = 1,
            .destination_port = 2,
            .protocol         = aloe::wire::Ipv4Protocol::Udp,
            .checksums        = aloe::frames::Checksums::Seeded
    },
        aloe::frames::pattern(16));
    auto lender = a.allocate(0);
    ASSERT_TRUE(lender.has_value());
    ASSERT_TRUE(aloe::frames::fill(*lender, frame));
    lender->set_tx({.l2_length          = 14,
                    .l3_length          = 20,
                    .fill_ipv4_checksum = true,
                    .fill_l4_checksum   = aloe::device::L4Checksum::Udp});
    std::array<Packet, 1> burst{lender->lend()};
    ASSERT_EQ(a.transmit(0, burst), 1);

    const auto after = aloe::frames::bytes_of(*lender);
    EXPECT_NE(after, frame) << "the fill completed the checksums in the lent block";
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const bool checksum_byte =
            index == 24 || index == 25 || index == 40 || index == 41;  // IPv4 at 14+10, UDP at 14+20+6
        if (!checksum_byte) {
            EXPECT_EQ(after[index], frame[index]) << "byte " << index;
        }
    }
    const auto got = drain(b);
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0], after) << "the receiver's copy is the filled frame";
    EXPECT_EQ(a.reclaim(0), 1);
}
