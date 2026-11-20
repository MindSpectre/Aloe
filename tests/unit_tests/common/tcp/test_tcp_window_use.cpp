#include <aloe/tcp>
#include <aloe/wire>
#include <cstdint>

#include <gtest/gtest.h>

// WindowUse, Linux's `tcp_cwnd_validate` in bytes: at each send it keeps whether the window limited it and the most in
// flight, for the round that ends when SND.UNA reaches the SND.NXT of the last record.
namespace {

    using aloe::tcp::detail::WindowUse;
    using Sequence = aloe::wire::TcpSequence;

}  // namespace

TEST(TcpWindowUse, ALimitedSendIsRecorded) {
    WindowUse use;
    EXPECT_FALSE(use.limited());
    EXPECT_EQ(use.largest_flight(), 0U);
    use.on_send(Sequence{1000}, Sequence{2000}, 1000, false);
    EXPECT_FALSE(use.limited());
    EXPECT_EQ(use.largest_flight(), 1000U);
    // Within the round, with less in flight than the most recorded: a limited send is recorded all the same.
    use.on_send(Sequence{1000}, Sequence{2500}, 800, true);
    EXPECT_TRUE(use.limited());
    EXPECT_EQ(use.largest_flight(), 800U);
    // An unlimited send of more, in the same round, does not take it back.
    use.on_send(Sequence{1000}, Sequence{3000}, 2000, false);
    EXPECT_TRUE(use.limited());
    EXPECT_EQ(use.largest_flight(), 800U);
}

TEST(TcpWindowUse, AnUnlimitedSendKeepsTheLargestFlightOfTheRound) {
    WindowUse use;
    use.on_send(Sequence{1000}, Sequence{1100}, 100, false);
    use.on_send(Sequence{1000}, Sequence{1300}, 300, false);
    EXPECT_EQ(use.largest_flight(), 300U) << "more in flight than any send before";
    use.on_send(Sequence{1100}, Sequence{1400}, 300, false);
    EXPECT_EQ(use.largest_flight(), 300U);
    use.on_send(Sequence{1299}, Sequence{1450}, 151, false);
    EXPECT_EQ(use.largest_flight(), 300U) << "the round runs to 1300, the SND.NXT recorded with 300";
    EXPECT_FALSE(use.limited());
}

TEST(TcpWindowUse, ANewRoundStartsOver) {
    WindowUse use;
    use.on_send(Sequence{1000}, Sequence{15600}, 14600, true);
    use.on_send(Sequence{15000}, Sequence{15700}, 700, false);
    EXPECT_TRUE(use.limited()) << "SND.UNA is short of 15600: the round goes on";
    EXPECT_EQ(use.largest_flight(), 14600U);
    use.on_send(Sequence{15600}, Sequence{15800}, 200, false);
    EXPECT_FALSE(use.limited()) << "SND.UNA reached 15600: the round ended, and a send of less starts the next";
    EXPECT_EQ(use.largest_flight(), 200U);
}

TEST(TcpWindowUse, TheRoundEndsAcrossTheTopOfTheSequenceSpace) {
    WindowUse use;
    use.on_send(Sequence{0xfffff000U}, Sequence{0x00000100U}, 0x1100, true);
    use.on_send(Sequence{0xffffff00U}, Sequence{0x00000200U}, 0x300, false);
    EXPECT_TRUE(use.limited()) << "0xffffff00 is before 0x100: the round goes on";
    use.on_send(Sequence{0x00000100U}, Sequence{0x00000300U}, 0x200, false);
    EXPECT_FALSE(use.limited());
    EXPECT_EQ(use.largest_flight(), 0x200U);
}
