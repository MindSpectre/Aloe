#pragma once

namespace aloe::tcp::detail {

    /**
     * @brief A connection's RFC 2018 selective acknowledgement: whether SACK-permitted was negotiated.
     *
     * Negotiated as the timestamps are (`Timestamps`): the SYN or SYN-ACK offers the option when the configuration
     * allows it, and it is in use only when both ends sent it. An active open that ends before the peer answered
     * negotiated nothing.
     */
    class SackBlocks {
    public:
        /// What the SYN or SYN-ACK offers.
        void offer(const bool enabled) noexcept {
            in_use_ = enabled;
        }

        /// In use only when both ends sent the option.
        void answer(const bool peer_offered) noexcept {
            in_use_ = in_use_ && peer_offered;
        }

        /// The offer was never answered.
        void withdraw() noexcept {
            in_use_ = false;
        }

        /// In SynSent the SYN's offer; from the handshake on, the option both ends sent.
        [[nodiscard]] bool in_use() const noexcept {
            return in_use_;
        }

    private:
        bool in_use_ = false;
    };

}  // namespace aloe::tcp::detail
