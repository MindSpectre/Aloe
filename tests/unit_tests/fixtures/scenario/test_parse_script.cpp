#include <aloe/scenario>
#include <aloe/wire>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using namespace std::chrono_literals;
    using aloe::scenario::Direction;
    using aloe::scenario::PacketLine;
    using aloe::scenario::parse_script;
    using aloe::scenario::Script;
    using aloe::scenario::TcpOption;
    using aloe::scenario::TimeSpec;
    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;

    const PacketLine& packet(const Script& script, const std::size_t index) {
        return std::get<PacketLine>(script.events.at(index).action);
    }

}  // namespace

TEST(ParseScript, ParsesEveryTimeForm) {
    const auto script = parse_script("+0 < S 0:0(0) win 65535 <mss 1460>\n"
                                     "+.01 > S. 0:0(0) ack 1 <mss 1460>\n"
                                     "0.5 < . 1:1(0) ack 1 win 257\n"
                                     "+1~+2 > . 1:1(0) ack 2\n"
                                     "3~4.25 > F. 1:1(0) ack 2\n"
                                     "* > R 2:2(0)\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 6);
    EXPECT_EQ(script->events[0].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 0s, .high = 0s}));
    EXPECT_EQ(script->events[1].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 10ms, .high = 10ms}));
    EXPECT_EQ(script->events[2].time, (TimeSpec{.kind = TimeSpec::Kind::Absolute, .low = 500ms, .high = 500ms}));
    EXPECT_EQ(script->events[3].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 1s, .high = 2s}));
    EXPECT_EQ(script->events[4].time, (TimeSpec{.kind = TimeSpec::Kind::Absolute, .low = 3s, .high = 4250ms}));
    EXPECT_EQ(script->events[5].time.kind, TimeSpec::Kind::Any);
    EXPECT_EQ(script->events[0].line, 1);
    EXPECT_EQ(script->events[5].line, 6);
    EXPECT_FALSE(script->tolerance.has_value());
}

TEST(ParseScript, ParsesFlagsNumbersAndFields) {
    const auto script = parse_script("+0 > P. 1:1001(1000) ack 1 win 46720 urg 7\n"
                                     "+0 < . 1:1(0) ack 1001\n"
                                     "+0 > R 5:5(0)\n"
                                     "+0 < - 1:1(0)\n"
                                     "+0 < F. 1001:1001(0) ack 1 win 257\n"
                                     "+0 < S 0:0(0) win 65535\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const PacketLine& data = packet(*script, 0);
    EXPECT_EQ(data.direction, Direction::Outbound);
    EXPECT_EQ(data.flags, (TcpFlags{TcpFlag::Psh, TcpFlag::Ack}));
    EXPECT_EQ(data.first, 1);
    EXPECT_EQ(data.last, 1001);
    EXPECT_EQ(data.length, 1000);
    EXPECT_EQ(data.ack, 1);
    EXPECT_EQ(data.window, 46720);
    EXPECT_EQ(data.urgent, 7);
    EXPECT_FALSE(data.options.any);
    EXPECT_TRUE(data.options.list.empty());

    EXPECT_EQ(packet(*script, 1).direction, Direction::Inbound);
    EXPECT_EQ(packet(*script, 1).flags, (TcpFlags{TcpFlag::Ack}));
    EXPECT_FALSE(packet(*script, 1).window.has_value());
    EXPECT_EQ(packet(*script, 2).flags, (TcpFlags{TcpFlag::Rst}));
    EXPECT_FALSE(packet(*script, 2).ack.has_value());
    EXPECT_EQ(packet(*script, 3).flags, TcpFlags{});
    EXPECT_EQ(packet(*script, 4).flags, (TcpFlags{TcpFlag::Fin, TcpFlag::Ack}));
    EXPECT_EQ(packet(*script, 5).flags, (TcpFlags{TcpFlag::Syn}));
}

TEST(ParseScript, ParsesEveryOption) {
    const auto script = parse_script(
        "+0 < S 0:0(0) win 65535 <mss 1000,sackOK,nop,nop,nop,wscale 7,TS val 100 ecr 200,sack 1:2,3:4,eol>\n"
        "+0 > S. 0:0(0) ack 1 <...>\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const auto& options = packet(*script, 0).options;
    EXPECT_FALSE(options.any);
    ASSERT_EQ(options.list.size(), 9);
    EXPECT_EQ(options.list[0], (TcpOption{.kind = TcpOption::Kind::Mss, .mss = 1000}));
    EXPECT_EQ(options.list[1].kind, TcpOption::Kind::SackPermitted);
    EXPECT_EQ(options.list[2].kind, TcpOption::Kind::Nop);
    EXPECT_EQ(options.list[5], (TcpOption{.kind = TcpOption::Kind::WindowScale, .shift = 7}));
    EXPECT_EQ(options.list[6], (TcpOption{.kind = TcpOption::Kind::Timestamps, .value = 100, .echo = 200}));
    EXPECT_EQ(options.list[7].kind, TcpOption::Kind::Sack);
    EXPECT_EQ(options.list[7].blocks,
              (std::vector<aloe::scenario::SackBlock>{
                  {.first = 1, .last = 2},
                  {.first = 3, .last = 4}
    }));
    EXPECT_EQ(options.list[8].kind, TcpOption::Kind::Eol);
    EXPECT_TRUE(packet(*script, 1).options.any);
    EXPECT_TRUE(packet(*script, 1).options.list.empty());
}

TEST(ParseScript, SkipsCommentsAndBlankLinesAndKeepsLineNumbers) {
    const auto script = parse_script("// A comment first.\n"
                                     "\n"
                                     "+0 < S 0:0(0) win 65535 <mss 1460>   // the SYN\n"
                                     "   \n"
                                     "// Another.\n"
                                     "+0 > S. 0:0(0) ack 1 <mss 1460>\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 2);
    EXPECT_EQ(script->events[0].line, 3);
    EXPECT_EQ(script->events[1].line, 6);
}

TEST(ParseScript, ReportsErrorsWithTheirLine) {
    const auto length = parse_script("+0 < S 0:0(0)\n+0 < P. 1:1001(999) ack 1\n");
    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error().line, 2);
    EXPECT_NE(length.error().message.find("length"), std::string_view::npos);

    const auto shell = parse_script("`../common/defaults.sh`\n+0 < S 0:0(0)\n");
    ASSERT_FALSE(shell.has_value());
    EXPECT_EQ(shell.error().line, 1);
    EXPECT_NE(shell.error().message.find("shell"), std::string_view::npos);

    const auto udp = parse_script("+0 < udp (1000)\n");
    ASSERT_FALSE(udp.has_value());
    EXPECT_NE(udp.error().message.find("not supported"), std::string_view::npos);

    const auto ip_info = parse_script("+0 < [ect0] S 0:0(0)\n");
    ASSERT_FALSE(ip_info.has_value());
    EXPECT_NE(ip_info.error().message.find("not supported"), std::string_view::npos);

    const auto bad_flag = parse_script("+0 < X 0:0(0)\n");
    ASSERT_FALSE(bad_flag.has_value());
    EXPECT_NE(bad_flag.error().message.find("flag"), std::string_view::npos);

    const auto bad_option = parse_script("+0 < S 0:0(0) <mss>\n");
    ASSERT_FALSE(bad_option.has_value());
    EXPECT_EQ(bad_option.error().line, 1);

    const auto no_time = parse_script("< S 0:0(0)\n");
    ASSERT_FALSE(no_time.has_value());
    EXPECT_NE(no_time.error().message.find("time"), std::string_view::npos);

    const auto trailing = parse_script("+0 < S 0:0(0) win 65535 junk\n");
    ASSERT_FALSE(trailing.has_value());
    EXPECT_NE(trailing.error().message.find("junk"), std::string_view::npos);
}

TEST(ParseScript, AnEmptyScriptHasNoEvents) {
    const auto script = parse_script("// nothing\n\n");
    ASSERT_TRUE(script.has_value());
    EXPECT_TRUE(script->events.empty());
}

TEST(ParseScript, ParsesCallLines) {
    const auto script = parse_script("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n"
                                     "+0 setsockopt(3, SOL_SOCKET, SO_REUSEADDR, [1], 4) = 0\n"
                                     "+0 fcntl(3, F_SETFL, O_RDWR|O_NONBLOCK) = 0\n"
                                     "+0 connect(3, ..., ...) = -1 EINPROGRESS (Operation now in progress)\n"
                                     "+0 write(4, ..., 1000) = 1000\n"
                                     "+32~+32.01 read(3, ..., 1) = -1 ETIMEDOUT\n"
                                     "+0 close(4) = 0\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_EQ(script->events.size(), 7);
    using aloe::scenario::Argument;
    using aloe::scenario::CallLine;
    const auto& socket = std::get<CallLine>(script->events[0].action);
    EXPECT_EQ(socket.name, "socket");
    ASSERT_EQ(socket.arguments.size(), 3);
    EXPECT_EQ(socket.arguments[0].kind, Argument::Kind::Ellipsis);
    EXPECT_EQ(socket.arguments[1], (Argument{.kind = Argument::Kind::Word, .text = "SOCK_STREAM"}));
    EXPECT_EQ(socket.result, 3);
    EXPECT_TRUE(socket.error.empty());
    EXPECT_EQ(script->events[0].time.kind, TimeSpec::Kind::Absolute);

    const auto& setsockopt = std::get<CallLine>(script->events[1].action);
    ASSERT_EQ(setsockopt.arguments.size(), 5);
    EXPECT_EQ(setsockopt.arguments[0], (Argument{.kind = Argument::Kind::Integer, .integer = 3}));
    EXPECT_EQ(setsockopt.arguments[3], (Argument{.kind = Argument::Kind::Bracketed, .text = "1"}));

    EXPECT_EQ(std::get<CallLine>(script->events[2].action).arguments[2],
              (Argument{.kind = Argument::Kind::Word, .text = "O_RDWR|O_NONBLOCK"}));

    const auto& connect = std::get<CallLine>(script->events[3].action);
    EXPECT_EQ(connect.result, -1);
    EXPECT_EQ(connect.error, "EINPROGRESS");
    EXPECT_EQ(connect.note, "Operation now in progress");

    const auto& read = std::get<CallLine>(script->events[5].action);
    EXPECT_EQ(read.result, -1);
    EXPECT_EQ(read.error, "ETIMEDOUT");
    EXPECT_TRUE(read.note.empty());
    EXPECT_EQ(script->events[5].time, (TimeSpec{.kind = TimeSpec::Kind::Relative, .low = 32s, .high = 32010ms}));

    const auto& close = std::get<CallLine>(script->events[6].action);
    EXPECT_EQ(close.name, "close");
    ASSERT_EQ(close.arguments.size(), 1);
    EXPECT_EQ(close.arguments[0].integer, 4);
}

TEST(ParseScript, ParsesAQuotedStringArgument) {
    const auto script = parse_script("+0 write(4, \"hello\", 5) = 5\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    const auto& write = std::get<aloe::scenario::CallLine>(script->events[0].action);
    EXPECT_EQ(write.arguments[1],
              (aloe::scenario::Argument{.kind = aloe::scenario::Argument::Kind::String, .text = "hello"}));
}

TEST(ParseScript, ParsesTheToleranceOption) {
    const auto script = parse_script("--tolerance_usecs=2000\n\n0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n");
    ASSERT_TRUE(script.has_value()) << script.error().message;
    EXPECT_EQ(script->tolerance, 2ms);
    EXPECT_EQ(script->events.size(), 1);
}

TEST(ParseScript, ReportsCallLineErrors) {
    const auto no_result = parse_script("+0 close(4)\n");
    ASSERT_FALSE(no_result.has_value());
    EXPECT_NE(no_result.error().message.find('='), std::string_view::npos);

    const auto unknown_option = parse_script("--mss=1000\n0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n");
    ASSERT_FALSE(unknown_option.has_value());
    EXPECT_EQ(unknown_option.error().line, 1);
    EXPECT_NE(unknown_option.error().message.find("mss"), std::string_view::npos);

    const auto late_option = parse_script("0 socket(..., SOCK_STREAM, IPPROTO_TCP) = 3\n--tolerance_usecs=1\n");
    ASSERT_FALSE(late_option.has_value());
    EXPECT_EQ(late_option.error().line, 2);

    const auto unclosed = parse_script("+0 close(4 = 0\n");
    ASSERT_FALSE(unclosed.has_value());
}
