#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::wire::TcpFlag;
    using aloe::wire::TcpFlags;
    using aloe::wire::TcpHeader;
    using aloe::wire::TcpOptions;
    using aloe::wire::TcpSequence;

    [[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
        std::vector<std::byte> out;
        for (const unsigned value : values) {
            out.push_back(std::byte{static_cast<std::uint8_t>(value)});
        }
        return out;
    }

    // Ports 80 -> 40000, sequence 1, acknowledgement 2, offset 20, ACK, window 65535, checksum 0, urgent 0.
    const std::vector<std::byte> ack_header =
        bytes({0x00, 0x50, 0x9c, 0x40, 0, 0, 0, 1, 0, 0, 0, 2, 0x50, 0x10, 0xff, 0xff, 0, 0, 0, 0});

}  // namespace

TEST(TcpSequence, WrapsAndCompares) {
    EXPECT_EQ((TcpSequence{0xfffffff0U} + 32U).value, 16U);
    EXPECT_EQ(TcpSequence{16U} - TcpSequence{0xfffffff0U}, 32);
    EXPECT_EQ(TcpSequence{0xfffffff0U} - TcpSequence{16U}, -32);
    EXPECT_TRUE(TcpSequence{0xfffffff0U}.before(TcpSequence{16U}));
    EXPECT_TRUE(TcpSequence{16U}.after(TcpSequence{0xfffffff0U}));
    EXPECT_FALSE(TcpSequence{5U}.before(TcpSequence{5U}));
    EXPECT_FALSE(TcpSequence{5U}.after(TcpSequence{5U}));
    EXPECT_EQ(TcpSequence{5U}, TcpSequence{5U});
    EXPECT_TRUE(TcpSequence{0U}.after(TcpSequence{0x80000001U}));  // half the space away: serial order
}

TEST(TcpFlags, SetQueryAndRaw) {
    const TcpFlags flags{TcpFlag::Syn, TcpFlag::Ack};
    EXPECT_TRUE(flags.has(TcpFlag::Syn));
    EXPECT_TRUE(flags.has(TcpFlag::Ack));
    EXPECT_FALSE(flags.has(TcpFlag::Fin));
    EXPECT_EQ(flags.raw(), 0x12U);
    EXPECT_EQ(TcpFlags::from_raw(0xffU).raw(), 0xffU);
    EXPECT_TRUE(TcpFlags::from_raw(0x80U).has(TcpFlag::Cwr));
    EXPECT_EQ(TcpFlags{}.raw(), 0U);
    EXPECT_EQ(TcpFlags::from_raw(0x12U), flags);
}

TEST(TcpHeader, SizeAndChecksumOffset) {
    EXPECT_EQ(TcpHeader::size, 20U);
    EXPECT_EQ(TcpHeader::checksum_offset, 16U);
}

TEST(TcpHeader, ParsesKnownBytes) {
    const auto header = TcpHeader::parse(ack_header);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->source_port, 80U);
    EXPECT_EQ(header->destination_port, 40000U);
    EXPECT_EQ(header->sequence, TcpSequence{1U});
    EXPECT_EQ(header->acknowledgement, TcpSequence{2U});
    EXPECT_EQ(header->data_offset, 20U);
    EXPECT_EQ(header->flags, (TcpFlags{TcpFlag::Ack}));
    EXPECT_EQ(header->window, 65535U);
    EXPECT_EQ(header->checksum, 0U);
    EXPECT_EQ(header->urgent_pointer, 0U);
}

TEST(TcpHeader, WritesKnownBytes) {
    std::array<std::byte, 20> out{};
    TcpHeader{.source_port      = 80,
              .destination_port = 40000,
              .sequence         = TcpSequence{1U},
              .acknowledgement  = TcpSequence{2U},
              .data_offset      = 20,
              .flags            = TcpFlags{TcpFlag::Ack},
              .window           = 65535}
        .write(out);
    EXPECT_EQ(std::vector<std::byte>(out.begin(), out.end()), ack_header);
}

TEST(TcpHeader, RoundTripWithOptionsAndEveryFlag) {
    std::array<std::byte, 24> out{};
    const TcpHeader written{.source_port      = 7,
                            .destination_port = 32768,
                            .sequence         = TcpSequence{0xdeadbeefU},
                            .acknowledgement  = TcpSequence{0x01020304U},
                            .data_offset      = 24,
                            .flags            = TcpFlags::from_raw(0xff),
                            .window           = 1,
                            .checksum         = 0xabcd,
                            .urgent_pointer   = 9};
    written.write(out);
    EXPECT_EQ(out[12], std::byte{0x60}) << "24 bytes is six words in the high nibble";
    const auto parsed = TcpHeader::parse(out);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, written);
}

TEST(TcpHeader, RejectsShortSegments) {
    for (std::size_t length = 0; length < 20; ++length) {
        EXPECT_FALSE(TcpHeader::parse(std::span<const std::byte>{ack_header}.first(length))) << length;
    }
}

TEST(TcpHeader, RejectsInvalidDataOffset) {
    auto under = ack_header;
    under[12]  = std::byte{0x40};  // four words: 16 bytes
    EXPECT_FALSE(TcpHeader::parse(under));
    auto beyond = ack_header;
    beyond[12]  = std::byte{0x60};  // 24 bytes, but the segment is 20
    EXPECT_FALSE(TcpHeader::parse(beyond));
    auto exact = ack_header;
    exact.resize(24);
    exact[12] = std::byte{0x60};
    EXPECT_TRUE(TcpHeader::parse(exact)) << "an offset equal to the segment length is a header without data";
}

TEST(TcpOptions, ParsesMssAndSkipsUnknown) {
    EXPECT_EQ(TcpOptions::mss_size, 4U);
    const auto mss = TcpOptions::parse(bytes({2, 4, 0x05, 0xb4}));
    ASSERT_TRUE(mss.has_value());
    EXPECT_EQ(mss->mss, 1460U);
    const auto small = TcpOptions::parse(bytes({2, 4, 0x02, 0x18}));
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(small->mss, 536U);
    // A Linux SYN: MSS, SACK permitted, timestamps, NOP, window scale, then EOL padding.
    const auto linux = TcpOptions::parse(bytes({2, 4, 0x05, 0xb4, 4, 2, 8, 10, 1, 2, 3, 4, 0, 0, 0, 0, 1, 3, 3, 7, 0}));
    ASSERT_TRUE(linux.has_value());
    EXPECT_EQ(linux->mss, 1460U);
    const auto none = TcpOptions::parse(bytes({1, 1, 0}));
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->mss.has_value());
    const auto empty = TcpOptions::parse({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_FALSE(empty->mss.has_value());
    const auto after_eol = TcpOptions::parse(bytes({0, 2, 4, 0x05, 0xb4}));
    ASSERT_TRUE(after_eol.has_value());
    EXPECT_FALSE(after_eol->mss.has_value()) << "nothing after EOL is read";
}

TEST(TcpOptions, RejectsBadLengths) {
    EXPECT_FALSE(TcpOptions::parse(bytes({2, 4, 0x05})));        // MSS truncated
    EXPECT_FALSE(TcpOptions::parse(bytes({2, 3, 0x05, 0xb4})));  // MSS with the wrong length
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 0, 1, 2})));        // length 0
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 1, 1, 2})));        // length 1
    EXPECT_FALSE(TcpOptions::parse(bytes({8, 10, 1, 2})));       // length past the option area
    EXPECT_FALSE(TcpOptions::parse(bytes({8})));                 // a kind with no length byte
}

TEST(TcpOptions, WritesMss) {
    std::array<std::byte, 4> out{};
    TcpOptions::write_mss(out, 1460);
    EXPECT_EQ(std::vector<std::byte>(out.begin(), out.end()), bytes({2, 4, 0x05, 0xb4}));
    const auto back = TcpOptions::parse(out);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->mss, 1460U);
}
