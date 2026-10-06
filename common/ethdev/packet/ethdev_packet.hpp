#pragma once

#include <algorithm>
#include <aloe/core>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include <packet.hpp>
#include <rte_mbuf.h>

namespace aloe::ethdev {

    /**
     * @brief The DPDK backend's packet: a move-only handle over one `rte_mbuf`.
     *
     * Models IsPacket at zero overhead. The mbuf is one segment; destroying the packet frees it to
     * its pool. Receive metadata is read from the mbuf's offload flags, and transmit metadata is
     * written to them, so there is no state beyond the pointer.
     */
    class Packet {
    public:
        Packet() noexcept = default;

        /// Adopts an mbuf, which must be a single segment.
        explicit Packet(rte_mbuf* mbuf) noexcept
            : mbuf_{mbuf} {
        }

        Packet(Packet&& other) noexcept
            : mbuf_{std::exchange(other.mbuf_, nullptr)} {
        }

        Packet& operator=(Packet&& other) noexcept {
            if (this != &other) {
                reset();
                mbuf_ = std::exchange(other.mbuf_, nullptr);
            }
            return *this;
        }

        Packet(const Packet&)            = delete;
        Packet& operator=(const Packet&) = delete;

        ~Packet() {
            reset();
        }

        [[nodiscard]] bool empty() const noexcept {
            return mbuf_ == nullptr;
        }

        [[nodiscard]] std::span<std::byte> data() noexcept {
            if (mbuf_ == nullptr) {
                return {};
            }
            return std::span{static_cast<std::byte*>(mbuf_->buf_addr) + mbuf_->data_off, mbuf_->data_len};
        }

        [[nodiscard]] std::span<const std::byte> data() const noexcept {
            if (mbuf_ == nullptr) {
                return {};
            }
            return std::span{static_cast<const std::byte*>(mbuf_->buf_addr) + mbuf_->data_off, mbuf_->data_len};
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return mbuf_ == nullptr ? 0 : mbuf_->data_len;
        }

        [[nodiscard]] std::size_t headroom() const noexcept {
            return mbuf_ == nullptr ? 0 : rte_pktmbuf_headroom(mbuf_);
        }

        [[nodiscard]] std::size_t tailroom() const noexcept {
            return mbuf_ == nullptr ? 0 : rte_pktmbuf_tailroom(mbuf_);
        }

        [[nodiscard]] std::optional<std::span<std::byte>> prepend(const std::size_t count) noexcept {
            if (mbuf_ == nullptr || count > std::numeric_limits<std::uint16_t>::max()) {
                return std::nullopt;
            }
            if (const char* front = rte_pktmbuf_prepend(mbuf_, static_cast<std::uint16_t>(count)); front == nullptr) {
                return std::nullopt;
            }
            return data().first(count);
        }

        [[nodiscard]] std::optional<std::span<std::byte>> append(const std::size_t count) noexcept {
            if (mbuf_ == nullptr || count > std::numeric_limits<std::uint16_t>::max()) {
                return std::nullopt;
            }
            if (const char* back = rte_pktmbuf_append(mbuf_, static_cast<std::uint16_t>(count)); back == nullptr) {
                return std::nullopt;
            }
            return data().last(count);
        }

        void trim_front(const std::size_t count) noexcept {
            assert(count <= size());
            core::force_non_const(this);  // writes the mbuf, which a const member could still do
            if (mbuf_ != nullptr) {
                std::ignore = rte_pktmbuf_adj(mbuf_, static_cast<std::uint16_t>(std::min(count, size())));
            }
        }

        void trim_back(const std::size_t count) noexcept {
            assert(count <= size());
            core::force_non_const(this);  // writes the mbuf, which a const member could still do
            if (mbuf_ != nullptr) {
                std::ignore = rte_pktmbuf_trim(mbuf_, static_cast<std::uint16_t>(std::min(count, size())));
            }
        }

        [[nodiscard]] device::RxMetadata rx() const noexcept {
            device::RxMetadata rx;
            if (mbuf_ == nullptr) {
                return rx;
            }
            const std::uint64_t flags = mbuf_->ol_flags;
            if ((flags & RTE_MBUF_F_RX_RSS_HASH) != 0) {
                rx.rss_hash = mbuf_->hash.rss;
            }
            rx.l3 =
                verdict(flags & RTE_MBUF_F_RX_IP_CKSUM_MASK, RTE_MBUF_F_RX_IP_CKSUM_GOOD, RTE_MBUF_F_RX_IP_CKSUM_BAD);
            rx.l4 =
                verdict(flags & RTE_MBUF_F_RX_L4_CKSUM_MASK, RTE_MBUF_F_RX_L4_CKSUM_GOOD, RTE_MBUF_F_RX_L4_CKSUM_BAD);
            return rx;
        }

        [[nodiscard]] device::TxMetadata tx() const noexcept {
            device::TxMetadata tx;
            if (mbuf_ == nullptr) {
                return tx;
            }
            const std::uint64_t flags = mbuf_->ol_flags;
            tx.l2_length              = static_cast<std::uint8_t>(mbuf_->l2_len);
            tx.l3_length              = static_cast<std::uint8_t>(mbuf_->l3_len);
            tx.fill_ipv4_checksum     = (flags & RTE_MBUF_F_TX_IP_CKSUM) != 0;
            if (const std::uint64_t l4 = flags & RTE_MBUF_F_TX_L4_MASK; l4 == RTE_MBUF_F_TX_TCP_CKSUM) {
                tx.fill_l4_checksum = device::L4Checksum::Tcp;
            } else if (l4 == RTE_MBUF_F_TX_UDP_CKSUM) {
                tx.fill_l4_checksum = device::L4Checksum::Udp;
            }
            return tx;
        }

        void set_tx(const device::TxMetadata& tx) noexcept {
            core::force_non_const(this);  // writes the mbuf, which a const member could still do
            if (mbuf_ == nullptr) {
                return;
            }
            // The lengths are bit fields; masking keeps GCC's -Wconversion quiet about the narrowing.
            mbuf_->l2_len = tx.l2_length & ((1U << RTE_MBUF_L2_LEN_BITS) - 1U);
            mbuf_->l3_len = tx.l3_length & ((1U << RTE_MBUF_L3_LEN_BITS) - 1U);
            std::uint64_t flags =
                mbuf_->ol_flags & ~(RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_L4_MASK);
            if (tx.fill_ipv4_checksum) {
                flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
            }
            if (tx.fill_l4_checksum == device::L4Checksum::Tcp) {
                flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_CKSUM;
            } else if (tx.fill_l4_checksum == device::L4Checksum::Udp) {
                flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM;
            }
            mbuf_->ol_flags = flags;
        }

        /// The mbuf, still owned by the packet.
        [[nodiscard]] rte_mbuf* get() const noexcept {
            return mbuf_;
        }

        /// Gives up ownership of the mbuf and leaves the packet empty.
        [[nodiscard]] rte_mbuf* release() noexcept {
            return std::exchange(mbuf_, nullptr);
        }

    private:
        [[nodiscard]] static constexpr device::ChecksumVerdict
        verdict(const std::uint64_t bits, const std::uint64_t good, const std::uint64_t bad) noexcept {
            if (bits == good) {
                return device::ChecksumVerdict::Good;
            }
            if (bits == bad) {
                return device::ChecksumVerdict::Bad;
            }
            return device::ChecksumVerdict::Unknown;
        }

        void reset() noexcept {
            if (mbuf_ != nullptr) {
                rte_pktmbuf_free(mbuf_);
                mbuf_ = nullptr;
            }
        }

        rte_mbuf* mbuf_ = nullptr;
    };

}  // namespace aloe::ethdev
