#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aloe::device {

    /// Bytes of headroom every freshly allocated packet has in front of its data, on every backend.
    inline constexpr std::size_t packet_headroom = 128;

    /// What a device found out about a checksum on receive.
    enum class ChecksumVerdict : std::uint8_t { Unknown, Good, Bad };

    /// Which L4 checksum a device is asked to complete on transmit.
    enum class L4Checksum : std::uint8_t { None, Tcp, Udp };

    /// Filled by the device on receive.
    struct RxMetadata {
        std::optional<std::uint32_t> rss_hash;          ///< Present when the device hashed the frame.
        ChecksumVerdict l3 = ChecksumVerdict::Unknown;  ///< IPv4 header checksum.
        ChecksumVerdict l4 = ChecksumVerdict::Unknown;  ///< TCP or UDP checksum.

        friend constexpr bool operator==(const RxMetadata&, const RxMetadata&) noexcept = default;
    };

    /**
     * @brief Read by the device on transmit.
     *
     * A packet that requests `fill_l4_checksum` has the IPv4 pseudo-header sum already written into
     * the L4 checksum field, and the device completes the sum over the L4 segment. A packet that
     * requests `fill_ipv4_checksum` has zero in the IPv4 checksum field. Requesting a fill on a
     * device whose capabilities do not offer it is a programming error.
     */
    struct TxMetadata {
        std::uint8_t l2_length      = 0;  ///< Ethernet header length.
        std::uint8_t l3_length      = 0;  ///< IPv4 header length.
        bool fill_ipv4_checksum     = false;
        L4Checksum fill_l4_checksum = L4Checksum::None;

        friend constexpr bool operator==(const TxMetadata&, const TxMetadata&) noexcept = default;
    };

    /**
     * @brief A move-only owning handle over one contiguous buffer: headroom, data, tailroom.
     *
     * Destroying a packet returns its buffer to the pool it came from. A default-constructed or
     * moved-from packet is empty and owns nothing. Growth returns the new bytes, or nothing when the
     * room is too small; nothing else can fail. `trim_front` and `trim_back` must not exceed
     * `size()`.
     *
     * The loan: `lend()` returns a second handle over the same buffer, for the device to transmit, and
     * `lent()` is true while that handle is alive, in a transmit ring, in the driver, or dropped nowhere
     * yet. The lender keeps its handle and never gives it up, so there is no ticket to redeem. The
     * contract: `lend()` on a packet that is not empty and not lent, after the frame is complete and
     * `set_tx` has run, one handle at a time; while lent, the lender reads (`data()`, `size()`, `rx()`,
     * `tx()`) and never grows, trims or sets metadata, and neither does anyone holding the handle; the
     * handle is transmitted or dropped, and dropping it untransmitted returns the loan at once; dropping
     * the lender while lent is allowed, and the buffer is freed exactly once when the last handle goes.
     * A device may complete the checksums it was asked to fill inside the lent buffer; the size, the
     * offsets and every other byte stay as lent. `lent()` is a poll: no backend has an event for the return.
     */
    template <typename P>
    concept IsPacket = std::movable<P> && std::default_initializable<P> &&
                       requires(P packet, const P const_packet, std::size_t count, TxMetadata tx) {
                           { const_packet.empty() } -> std::same_as<bool>;
                           { packet.data() } -> std::same_as<std::span<std::byte>>;
                           { const_packet.size() } -> std::same_as<std::size_t>;
                           { const_packet.headroom() } -> std::same_as<std::size_t>;
                           { const_packet.tailroom() } -> std::same_as<std::size_t>;
                           { packet.prepend(count) } -> std::same_as<std::optional<std::span<std::byte>>>;
                           { packet.append(count) } -> std::same_as<std::optional<std::span<std::byte>>>;
                           { packet.trim_front(count) } -> std::same_as<void>;
                           { packet.trim_back(count) } -> std::same_as<void>;
                           { const_packet.rx() } -> std::same_as<RxMetadata>;
                           { const_packet.tx() } -> std::same_as<TxMetadata>;
                           { packet.set_tx(tx) } -> std::same_as<void>;
                           { packet.lend() } -> std::same_as<P>;
                           { const_packet.lent() } -> std::same_as<bool>;
                       };

}  // namespace aloe::device
