#pragma once

#include <aloe/core>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <stream_events.hpp>
#include <stream_wait.hpp>
#include <tcp_config.hpp>

namespace aloe::runtime {

    template <typename Stack>
    class Streams;

    namespace detail {
        template <typename Stack, typename Policy>
        struct StreamSender;
        struct ReadablePolicy;
        struct WritablePolicy;
        struct AckedPolicy;
        struct ClosedPolicy;
        struct SendPolicy;
    }  // namespace detail

    /**
     * @brief A move-only owner of one connection: the synchronous stream members, the senders that
     * wait for its events, and `release` in the destructor so a task that returns for any reason
     * frees the slot.
     *
     * The synchronous `send` and `close` of the brick are reached through `connection()`; the
     * members here with those names are senders. A moved-from or released handle is inert.
     */
    template <typename Stack>
    class Stream {
    public:
        using ConnectionType = typename Stack::ConnectionType;
        using Sequence       = typename Stack::Sequence;
        using View           = typename ConnectionType::View;

        Stream() noexcept = default;

        Stream(Streams<Stack>& owner, const std::uint32_t slot) noexcept
            : owner_{&owner},
              index_{slot} {
            owner.own(slot);
        }

        Stream(Stream&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)},
              index_{other.index_} {
        }

        Stream& operator=(Stream&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        Stream(const Stream&)            = delete;
        Stream& operator=(const Stream&) = delete;

        ~Stream() {
            release();
        }

        [[nodiscard]] bool valid() const noexcept {
            return owner_ != nullptr;
        }
        [[nodiscard]] std::uint32_t index() const noexcept {
            return index_;
        }
        [[nodiscard]] ConnectionType& connection() noexcept {
            return owner_->tcp().connection(index_);
        }
        [[nodiscard]] const ConnectionType& connection() const noexcept {
            return owner_->tcp().connection(index_);
        }

        [[nodiscard]] View unread() const noexcept {
            return connection().unread();
        }
        void consume(const std::size_t count) noexcept {
            connection().consume(count);
        }
        [[nodiscard]] bool peer_closed() const noexcept {
            return connection().peer_closed();
        }
        [[nodiscard]] std::size_t writable() const noexcept {
            return connection().writable();
        }
        [[nodiscard]] std::optional<std::span<std::byte>> prepare(const std::size_t count) noexcept {
            return connection().prepare(count);
        }
        [[nodiscard]] bool commit(const std::size_t count) noexcept {
            return connection().commit(count);
        }
        [[nodiscard]] Sequence committed() const noexcept {
            return connection().committed();
        }
        [[nodiscard]] Sequence acknowledged() const noexcept {
            return connection().acknowledged();
        }
        [[nodiscard]] std::size_t unacknowledged() const noexcept {
            return connection().unacknowledged();
        }
        [[nodiscard]] stream::Events events() const noexcept {
            return connection().events();
        }
        [[nodiscard]] tcp::State state() const noexcept {
            return connection().state();
        }
        [[nodiscard]] tcp::Endpoint local() const noexcept {
            return connection().local();
        }
        [[nodiscard]] tcp::Endpoint remote() const noexcept {
            return connection().remote();
        }
        [[nodiscard]] std::uint16_t mss() const noexcept {
            return connection().mss();
        }

        /// `unread().size() >= count`, the bytes; `PeerClosed` with fewer; `Reset`, `TimedOut`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ReadablePolicy> readable(const std::size_t count) noexcept {
            return {owner_, index_, count, {}};
        }
        /// `writable() >= count`, at most the MSS, the credit; `Closed` once closed.
        [[nodiscard]] detail::StreamSender<Stack, detail::WritablePolicy> writable(const std::size_t count) noexcept {
            return {owner_, index_, count, {}};
        }
        /// `acknowledged()` reaches `sequence`.
        [[nodiscard]] detail::StreamSender<Stack, detail::AckedPolicy> acked(const Sequence sequence) noexcept {
            return {owner_, index_, sequence.value, {}};
        }
        /// A normal `Closed`; `Reset`, `TimedOut`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ClosedPolicy> closed() noexcept {
            return {owner_, index_, 0, {.close_first = false}};
        }
        /// Commits all of `bytes`, parking on writable between segments; `bytes` must outlive the operation.
        [[nodiscard]] detail::StreamSender<Stack, detail::SendPolicy>
        send(const std::span<const std::byte> bytes) noexcept {
            return {owner_, index_, 1, {.bytes = bytes}};
        }
        /// The brick's close, then `closed()`.
        [[nodiscard]] detail::StreamSender<Stack, detail::ClosedPolicy> close() noexcept {
            return {owner_, index_, 0, {.close_first = true}};
        }

        void cancel() noexcept {
            owner_->cancel(index_);
        }
        void deadline(const core::TimePoint when) noexcept {
            owner_->deadline(index_, when);
        }

        /// Frees the slot through the owner; idempotent; the handle is inert afterwards.
        void release() noexcept {
            if (owner_ != nullptr) {
                owner_->release(index_);
                owner_ = nullptr;
            }
        }

    private:
        Streams<Stack>* owner_ = nullptr;
        std::uint32_t index_   = 0;
    };

}  // namespace aloe::runtime
