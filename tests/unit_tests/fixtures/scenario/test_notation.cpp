#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::describe;
    using aloe::scenario::describe_options;
    using aloe::scenario::encode_options;
    using aloe::scenario::format_time;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::wire::TcpSequence;

    PacketLine line_of(const char* text) {
        const auto script = parse_script(text);
        EXPECT_TRUE(script.has_value()) << script.error().message;
        return std::get<PacketLine>(script->events.at(0).action);
    }

    std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

}  // namespace

TEST(Notation, EncodesEveryOptionKind) {
    const PacketLine line = line_of("+0 < S 0:0(0) <mss 1460,nop,eol,sackOK,wscale 7,TS val 1 ecr 2,sack 1:2 3:4>\n");
    EXPECT_EQ(encode_options(line.options, TcpSequence{1000}),
              bytes({2, 4, 0x05, 0xb4, 1, 0, 4,    2, 3, 3, 7,    8, 10, 0, 0,    0, 1, 0, 0,   0,
                     2, 5, 18,   0,    0, 3, 0xe9, 0, 0, 3, 0xea, 0, 0,  3, 0xeb, 0, 0, 3, 0xec}));
}

TEST(Notation, DescribesOptionBytesInTheScriptsWords) {
    EXPECT_EQ(describe_options(bytes({2, 4, 0x05, 0xb4, 1, 1, 4, 2}), TcpSequence{}), "mss 1460,nop,nop,sackOK");
    EXPECT_EQ(describe_options(bytes({3, 3, 7}), TcpSequence{}), "wscale 7");
    EXPECT_EQ(describe_options(bytes({8, 10, 0, 0, 0, 1, 0, 0, 0, 2}), TcpSequence{}), "TS val 1 ecr 2");
    EXPECT_EQ(describe_options(bytes({5, 10, 0, 0, 3, 0xe9, 0, 0, 3, 0xea}), TcpSequence{1000}), "sack 1:2");
    EXPECT_EQ(
        describe_options(bytes({5, 18, 0, 0, 3, 0xe9, 0, 0, 3, 0xea, 0, 0, 3, 0xeb, 0, 0, 3, 0xec}), TcpSequence{1000}),
        "sack 1:2 3:4");
    EXPECT_EQ(describe_options(bytes({30, 2}), TcpSequence{}), "kind 30");
    EXPECT_EQ(describe_options(bytes({2, 4, 0x05}), TcpSequence{}), "bad options");
    EXPECT_EQ(describe_options({}, TcpSequence{}), "");
}

TEST(Notation, DescribesALineAsWritten) {
    EXPECT_EQ(describe(line_of("+0 > S. 0:0(0) ack 1 win 46720 <mss 1460>\n"), TcpSequence{}),
              "> S. 0:0(0) ack 1 win 46720 <mss 1460>");
    EXPECT_EQ(describe(line_of("+0 < P. 1:1001(1000) ack 1\n"), TcpSequence{}), "< P. 1:1001(1000) ack 1");
    EXPECT_EQ(describe(line_of("+0 > R 2:2(0)\n"), TcpSequence{}), "> R 2:2(0)");
    EXPECT_EQ(describe(line_of("+0 > . 1:1(0) ack 1 <...>\n"), TcpSequence{}), "> . 1:1(0) ack 1 <...>");
    EXPECT_EQ(describe(line_of("+0 < - 1:1(0) urg 3\n"), TcpSequence{}), "< - 1:1(0) urg 3");
}

TEST(Notation, DescribesAnObservedSegment) {
    const aloe::scenario::ObservedSegment seen{
        .flags   = {aloe::wire::TcpFlag::Syn, aloe::wire::TcpFlag::Ack},
        .first   = 0,
        .last    = 0,
        .length  = 0,
        .ack     = 1,
        .window  = 46720,
        .urgent  = 0,
        .options = bytes({2, 4, 0x05, 0xb4}
            )
    };
    EXPECT_EQ(describe(seen, TcpSequence{}), "> S. 0:0(0) ack 1 win 46720 <mss 1460>");
    aloe::scenario::ObservedSegment plain = seen;
    plain.flags                           = {aloe::wire::TcpFlag::Ack};
    plain.options.clear();
    EXPECT_EQ(describe(plain, TcpSequence{}), "> . 0:0(0) ack 1 win 46720");
}

TEST(Notation, FormatsTimes) {
    EXPECT_EQ(format_time(10ms), "+0.010000");
    EXPECT_EQ(format_time(63s), "+63.000000");
    EXPECT_EQ(format_time(0s), "+0.000000");
}
