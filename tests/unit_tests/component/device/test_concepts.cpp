#include <algorithm>
#include <aloe/device>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

    // The smallest real types that satisfy the concepts, to pin what the concepts demand.
    class FakePacket {
    public:
        FakePacket()                             = default;
        FakePacket(FakePacket&&)                 = default;
        FakePacket& operator=(FakePacket&&)      = default;
        FakePacket(const FakePacket&)            = delete;
        FakePacket& operator=(const FakePacket&) = delete;
        ~FakePacket()                            = default;

        [[nodiscard]] bool empty() const noexcept {
            return storage_.empty();
        }
        [[nodiscard]] std::span<std::byte> data() noexcept {
            return std::span<std::byte>{storage_}.subspan(begin_, end_ - begin_);
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return end_ - begin_;
        }
        [[nodiscard]] std::size_t headroom() const noexcept {
            return begin_;
        }
        [[nodiscard]] std::size_t tailroom() const noexcept {
            return storage_.size() - end_;
        }
        [[nodiscard]] std::optional<std::span<std::byte>> prepend(std::size_t count) noexcept {
            if (count > headroom()) {
                return std::nullopt;
            }
            begin_ -= count;
            return data().first(count);
        }
        [[nodiscard]] std::optional<std::span<std::byte>> append(std::size_t count) noexcept {
            if (count > tailroom()) {
                return std::nullopt;
            }
            end_ += count;
            return data().last(count);
        }
        void trim_front(std::size_t count) noexcept {
            begin_ += std::min(count, size());
        }
        void trim_back(std::size_t count) noexcept {
            end_ -= std::min(count, size());
        }
        [[nodiscard]] aloe::RxMetadata rx() const noexcept {
            return rx_;
        }
        [[nodiscard]] aloe::TxMetadata tx() const noexcept {
            return tx_;
        }
        void set_tx(const aloe::TxMetadata& tx) noexcept {
            tx_ = tx;
        }

    private:
        std::vector<std::byte> storage_ = std::vector<std::byte>(aloe::packet_headroom + 64);
        std::size_t begin_              = aloe::packet_headroom;
        std::size_t end_                = aloe::packet_headroom;
        aloe::RxMetadata rx_;
        aloe::TxMetadata tx_;
    };

    class FakeDevice {
    public:
        using Packet = FakePacket;

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return static_cast<std::uint16_t>(counters_.size());
        }
        [[nodiscard]] aloe::MacAddress mac() const noexcept {
            return mac_;
        }
        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return capabilities_.max_mtu;
        }
        [[nodiscard]] bool link_up() const noexcept {
            return steering_.enabled;
        }
        [[nodiscard]] const aloe::Capabilities& capabilities() const noexcept {
            return capabilities_;
        }
        [[nodiscard]] const aloe::RssDescription& steering() const noexcept {
            return steering_;
        }
        [[nodiscard]] std::optional<Packet> allocate(std::uint16_t queue) noexcept {
            if (queue >= queue_count()) {
                return std::nullopt;
            }
            ++allocated_;
            return Packet{};
        }
        [[nodiscard]] std::size_t receive(std::uint16_t queue, std::span<Packet> out) noexcept {
            counters_[queue].received += out.size();
            return 0;
        }
        [[nodiscard]] std::size_t transmit(std::uint16_t queue, std::span<Packet> in) noexcept {
            counters_[queue].transmitted += in.size();
            return in.size();
        }
        [[nodiscard]] aloe::QueueCounters counters(std::uint16_t queue) const noexcept {
            return counters_[queue];
        }

    private:
        aloe::MacAddress mac_{0x02, 0, 0, 0, 0, 1};
        aloe::Capabilities capabilities_{.max_mtu = 1500};
        aloe::RssDescription steering_;
        std::array<aloe::QueueCounters, 1> counters_{};
        std::size_t allocated_ = 0;
    };

    struct NotAPacket {};

    static_assert(aloe::IsPacket<FakePacket>);
    static_assert(!aloe::IsPacket<NotAPacket>);
    static_assert(aloe::IsDevice<FakeDevice>);
    static_assert(!aloe::IsDevice<NotAPacket>);

}  // namespace

TEST(Concepts, MetadataDefaultsToUnknown) {
    constexpr aloe::RxMetadata rx;
    static_assert(!rx.rss_hash.has_value());
    static_assert(rx.l3 == aloe::ChecksumVerdict::Unknown);
    static_assert(rx.l4 == aloe::ChecksumVerdict::Unknown);

    constexpr aloe::TxMetadata tx;
    static_assert(!tx.fill_ipv4_checksum);
    static_assert(tx.fill_l4_checksum == aloe::L4Checksum::None);
    static_assert(aloe::packet_headroom == 128);
    SUCCEED();
}

TEST(Concepts, TheFakePacketGrowsAndShrinksAsTheConceptDescribes) {
    FakePacket packet;
    EXPECT_EQ(packet.size(), 0);
    EXPECT_EQ(packet.headroom(), aloe::packet_headroom);
    ASSERT_TRUE(packet.append(10).has_value());
    ASSERT_TRUE(packet.prepend(14).has_value());
    EXPECT_EQ(packet.size(), 24);
    EXPECT_FALSE(packet.prepend(aloe::packet_headroom).has_value());
    packet.trim_front(14);
    packet.trim_back(10);
    EXPECT_EQ(packet.size(), 0);
}
