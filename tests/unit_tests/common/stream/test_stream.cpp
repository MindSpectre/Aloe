#include <algorithm>
#include <aloe/stream>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

    using aloe::stream::Error;
    using aloe::stream::Event;
    using aloe::stream::Events;

    /// A read view over a fixed set of chunks: the shape the concept wants, with the byte count separate.
    class ChunkView {
    public:
        using Chunks   = std::vector<std::span<const std::byte>>;
        using iterator = Chunks::const_iterator;

        explicit ChunkView(Chunks chunks)
            : chunks_{std::move(chunks)} {
            for (const auto chunk : chunks_) {
                bytes_ += chunk.size();
            }
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return bytes_;
        }
        [[nodiscard]] bool empty() const noexcept {
            return bytes_ == 0;
        }
        [[nodiscard]] std::span<const std::byte> front() const noexcept {
            return chunks_.front();
        }
        [[nodiscard]] iterator begin() const noexcept {
            return chunks_.begin();
        }
        [[nodiscard]] iterator end() const noexcept {
            return chunks_.end();
        }

    private:
        Chunks chunks_;
        std::size_t bytes_ = 0;
    };

    /// A stream whose prepare gives at most `segment` bytes and whose commit can be told to refuse.
    class StubStream {
    public:
        std::size_t segment              = 3;
        std::size_t refuse_commit_number = 0;  ///< 1-based; 0 never refuses.
        std::size_t null_prepare_after   = 0;  ///< Prepares after this many succeed return nothing; 0 means never.
        std::vector<std::size_t> prepared_sizes;
        std::vector<std::size_t> committed_sizes;
        std::vector<std::byte> wire;

        [[nodiscard]] static std::uint32_t index() noexcept {
            return 0;
        }
        [[nodiscard]] static Events events() noexcept {
            return {};
        }
        [[nodiscard]] static ChunkView unread() noexcept {
            return ChunkView{{}};
        }
        void consume(std::size_t) noexcept {
        }
        [[nodiscard]] static bool peer_closed() noexcept {
            return false;
        }
        [[nodiscard]] std::size_t writable() const noexcept {
            return segment;
        }

        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            if (count == 0 || (null_prepare_after != 0 && prepared_sizes.size() >= null_prepare_after)) {
                return std::nullopt;
            }
            const std::size_t size = std::min(count, segment);
            prepared_sizes.push_back(size);
            buffer_.assign(size, std::byte{0});
            return std::span<std::byte>{buffer_};
        }

        [[nodiscard]] bool commit(const std::size_t count) noexcept {
            ++commits_;
            if (count == 0) {
                return true;
            }
            if (commits_ == refuse_commit_number) {
                return false;
            }
            committed_sizes.push_back(count);
            wire.insert(wire.end(), buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
            return true;
        }

        void close() noexcept {
        }
        void abort() noexcept {
        }
        void release() noexcept {
        }

    private:
        std::vector<std::byte> buffer_;
        std::size_t commits_ = 0;
    };

    static_assert(aloe::stream::IsReadView<ChunkView>);
    static_assert(aloe::stream::IsStream<StubStream>);
    static_assert(!aloe::stream::IsReadView<std::vector<std::byte>>, "a byte range is not a view of chunks");
    static_assert(!aloe::stream::IsStream<ChunkView>);

    [[nodiscard]] std::vector<std::byte> five() {
        return {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    }

}  // namespace

TEST(StreamEvents, DefaultEmptyNamedQueriesAndRaw) {
    const Events none;
    EXPECT_FALSE(none.any());
    EXPECT_EQ(none.raw(), 0U);
    Events some{Event::Readable, Event::Acked};
    EXPECT_TRUE(some.any());
    EXPECT_TRUE(some.readable());
    EXPECT_TRUE(some.acked());
    EXPECT_FALSE(some.writable());
    EXPECT_TRUE(some.has(Event::Readable));
    EXPECT_EQ(some.raw(), (1U << 2U) | (1U << 4U));
    some |= Event::PeerClosed;
    EXPECT_TRUE(some.peer_closed());
    some |= Events{Event::Closed, Event::Reset};
    EXPECT_TRUE(some.closed());
    EXPECT_TRUE(some.reset());
    EXPECT_FALSE(some.timed_out());
    EXPECT_EQ(Events::from_raw(1U << 8U), Events{Event::TimedOut});
    EXPECT_TRUE(Events{Event::Connected}.connected());
    EXPECT_TRUE(Events{Event::Accepted}.accepted());
}

TEST(StreamSend, SplitsAtWritable) {
    StubStream stub;
    const auto bytes = five();
    EXPECT_EQ(aloe::stream::send(stub, bytes), 5U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{3, 2}));
    EXPECT_EQ(stub.wire, bytes);
}

TEST(StreamSend, StopsOnRefusedCommit) {
    StubStream stub;
    stub.refuse_commit_number = 2;
    EXPECT_EQ(aloe::stream::send(stub, five()), 3U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{3}));
    EXPECT_EQ(stub.prepared_sizes.size(), 2U) << "no third attempt after a refusal";
}

TEST(StreamSend, StopsOnNullPrepare) {
    StubStream stub;
    stub.null_prepare_after = 1;
    EXPECT_EQ(aloe::stream::send(stub, five()), 3U);
    EXPECT_EQ(stub.prepared_sizes.size(), 1U);
}

TEST(StreamSend, EmptyInputMakesNoPrepare) {
    StubStream stub;
    EXPECT_EQ(aloe::stream::send(stub, {}), 0U);
    EXPECT_TRUE(stub.prepared_sizes.empty());
}

TEST(StreamSend, DoesNotCoalesceCalls) {
    StubStream stub;
    stub.segment = 10;
    const std::array<std::byte, 2> a{std::byte{1}, std::byte{2}};
    const std::array<std::byte, 2> b{std::byte{3}, std::byte{4}};
    EXPECT_EQ(aloe::stream::send(stub, a), 2U);
    EXPECT_EQ(aloe::stream::send(stub, b), 2U);
    EXPECT_EQ(stub.committed_sizes, (std::vector<std::size_t>{2, 2})) << "two calls, two segments";
}

TEST(StreamError, HasEveryValue) {
    EXPECT_NE(Error::Reset, Error::Closed);
    EXPECT_EQ(static_cast<int>(Error::Closed), 8)
        << "Reset, TimedOut, PeerClosed, Refused, Unplaceable, TableFull, NoPort, NoRoute, Closed";
}
