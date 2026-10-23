#include <aloe/scenario>
#include <aloe/wire>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::scenario::match;
    using aloe::scenario::ObservedSegment;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpSequence;

    PacketLine line_of(const char* text) {
        return std::get<PacketLine>(parse_script(text)->events.at(0).action);
    }

    ObservedSegment syn_ack() {
        return {
            .flags   = {TcpFlag::Syn, TcpFlag::Ack},
            .first   = 0,
            .last    = 0,
            .length  = 0,
            .ack     = 1,
            .window  = 46720,
            .urgent  = 0,
            .options = {std::byte{2}, std::byte{4}, std::byte{0x05}, std::byte{0xb4}}
        };
    }

}  // namespace

TEST(Match, EveryStatedFieldMustAgree) {
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"), syn_ack(), TcpSequence{}), std::nullopt);
    EXPECT_EQ(match(line_of("+0 > S 0:0(0) <mss 1460>\n"), syn_ack(), TcpSequence{}), "flags differ");
    EXPECT_EQ(match(line_of("+0 > S. 1:1(0) ack 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), "sequence differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 2 <mss 1460>\n"), syn_ack(), TcpSequence{}), "ack differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 win 100 <mss 1460>\n"), syn_ack(), TcpSequence{}), "window differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 urg 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), "urgent differs");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1000>\n"), syn_ack(), TcpSequence{}), "options differ");
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1\n"), syn_ack(), TcpSequence{}), "options differ")
        << "nothing written means no options";
}

TEST(Match, UnstatedFieldsAreNotCompared) {
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <...>\n"), syn_ack(), TcpSequence{}), std::nullopt) << "any options";
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1460>\n"), syn_ack(), TcpSequence{}), std::nullopt)
        << "no window written";
    ObservedSegment urgent = syn_ack();
    urgent.urgent          = 5;
    EXPECT_EQ(match(line_of("+0 > S. 0:0(0) ack 1 <mss 1460>\n"), urgent, TcpSequence{}), std::nullopt);
}

TEST(Match, LengthAndRangeBothCount) {
    ObservedSegment data{
        .flags   = {TcpFlag::Psh, TcpFlag::Ack},
        .first   = 1,
        .last    = 1001,
        .length  = 1000,
        .ack     = 1,
        .window  = 46720,
        .urgent  = 0,
        .options = {}
    };
    EXPECT_EQ(match(line_of("+0 > P. 1:1001(1000) ack 1\n"), data, TcpSequence{}), std::nullopt);
    EXPECT_EQ(match(line_of("+0 > P. 1:501(500) ack 1\n"), data, TcpSequence{}), "sequence differs");
}
