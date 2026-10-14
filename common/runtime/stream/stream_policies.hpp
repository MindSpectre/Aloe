#pragma once

#include <aloe/core>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <stream.hpp>
#include <stream_events.hpp>
#include <stream_wait.hpp>
#include <tcp_config.hpp>

namespace aloe::runtime {

    template <typename Stack>
    class Stream;

}  // namespace aloe::runtime

// The policies of the stream senders, one per kind of wait; the operation state that drives them is in
// stream_senders.hpp, and Streams builds its accept and connect senders from the last two.
namespace aloe::runtime::detail {

    template <typename Op>
    using Stack_of = Op::StackType;

    /**
     * The policies: what a sender checks when it starts, what it does when woken, and how it turns the
     * saved outcome into its value. `begin` returns an outcome to complete with at once, or nothing
     * to park. `resume` returns true when a woken operation completes, false when it parked again.
     */
    struct ReadablePolicy {
        using Result               = std::expected<std::size_t, stream::Error>;
        static constexpr auto kind = WaitKind::Readable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            if (c.unread().size() >= op.threshold) {
                return WaitOutcome{.value = c.unread().size()};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error == stream::Error::Closed ? stream::Error::PeerClosed : *error};
            }
            if (c.peer_closed()) {
                return WaitOutcome{.error = stream::Error::PeerClosed};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct WritablePolicy {
        using Result               = std::expected<std::size_t, stream::Error>;
        static constexpr auto kind = WaitKind::Writable;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c = op.owner->tcp().connection(op.index);
            assert(op.threshold <= c.mss() && "writable(n): n is at most the MSS");
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            if (c.writable() >= op.threshold) {
                return WaitOutcome{.value = c.writable()};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    struct AckedPolicy {
        using Result               = std::expected<void, stream::Error>;
        static constexpr auto kind = WaitKind::Acked;

        template <typename Op>
        [[nodiscard]] static std::optional<WaitOutcome> begin(Op& op) noexcept {
            const auto& c  = op.owner->tcp().connection(op.index);
            using Sequence = Stack_of<Op>::Sequence;
            if (!c.acknowledged().before(Sequence{static_cast<std::uint32_t>(op.threshold)})) {
                return WaitOutcome{};
            }
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `closed()`, and `close()` when `close_first`: the brick's close, then the wait.
    struct ClosedPolicy {
        using Result               = std::expected<void, stream::Error>;
        static constexpr auto kind = WaitKind::Closed;

        bool close_first = false;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (close_first) {
                c.close();
            }
            if (const auto error = op.owner->terminal(op.index)) {
                if (*error == stream::Error::Closed) {
                    return WaitOutcome{};
                }
                return WaitOutcome{.error = *error};
            }
            if (c.state() == tcp::State::Closed) {
                // Closed with no terminal recorded: the terminal event is raised and not yet drained, as when a
                // retransmit timer times out inside run_once and the chain that runs next starts this sender.
                // Read it from the undrained flags, mapped as the wake pass maps it for a Closed wait.
                const stream::Events pending = c.events();
                if (pending.timed_out()) {
                    return WaitOutcome{.error = stream::Error::TimedOut};
                }
                if (pending.unresolved()) {
                    return WaitOutcome{.error = stream::Error::Unresolved};
                }
                if (pending.reset()) {
                    return WaitOutcome{.error = stream::Error::Reset};  // a handed-over stream is past Connected
                }
                if (pending.closed()) {
                    return WaitOutcome{};
                }
                // Nothing pending: every path to Closed raises a terminal event, so the next wake delivers it.
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return {};
        }
    };

    /// `send(bytes)`: commits what it can, parks on Writable for the rest, keeps the caller's span until done.
    struct SendPolicy {
        using Result               = std::expected<std::size_t, stream::Error>;
        static constexpr auto kind = WaitKind::Writable;

        std::span<const std::byte> bytes{};
        std::size_t accepted = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            op.threshold = 1;
            return advance(op);
        }

        template <typename Op>
        [[nodiscard]] bool resume(Op& op) noexcept {
            if (op.outcome.stopped || op.outcome.error) {
                return true;
            }
            const std::optional<WaitOutcome> done = advance(op);
            if (!done) {
                op.owner->park(op);  // a refused retry, or more to send: park again under the same callbacks
                return false;
            }
            op.outcome = *done;
            return true;
        }

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> advance(Op& op) noexcept {
            auto& c = op.owner->tcp().connection(op.index);
            if (const auto error = op.owner->terminal(op.index)) {
                return WaitOutcome{.error = *error};
            }
            accepted += stream::send(c, bytes.subspan(accepted));
            if (accepted == bytes.size()) {
                return WaitOutcome{.value = accepted};
            }
            return std::nullopt;  // a refused prepare or commit, or no credit: wait for the hint, never retry inline
        }

        template <typename Op>
        [[nodiscard]] static Result result(const Op& op) noexcept {
            if (op.outcome.error) {
                return std::unexpected{*op.outcome.error};
            }
            return op.outcome.value;
        }
    };

    [[nodiscard]] constexpr stream::Error to_error(const tcp::ConnectError error) noexcept {
        switch (error) {
            case tcp::ConnectError::TableFull:
                return stream::Error::TableFull;
            case tcp::ConnectError::NoPort:
                return stream::Error::NoPort;
            case tcp::ConnectError::Unplaceable:
                return stream::Error::Unplaceable;
            default:
                return stream::Error::NoRoute;
        }
    }

    /// `connect(peer)`: opens on start and owns the pending connection until it hands it over.
    struct ConnectPolicy {
        template <typename Stack>
        using ResultFor            = std::expected<Stream<Stack>, stream::Error>;
        static constexpr auto kind = WaitKind::Connected;

        tcp::Endpoint peer{};
        std::optional<core::TimePoint> deadline{};  ///< Arms the slot's timer once the connection has a slot.
        bool opened = false;  ///< A SynSent connection this operation owns until it hands it over.

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) noexcept {
            auto& stack          = op.owner->tcp();
            const auto connected = stack.connect(peer, op.owner->context().now());
            if (!connected) {
                return WaitOutcome{.error = to_error(connected.error())};
            }
            op.index = (*connected)->index();
            opened   = true;
            if (deadline) {
                op.owner->deadline(op.index, *deadline);  // the slot's timer; its fire is cancel
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A stopped or failed open releases the connection it still owns; a handed-over one is the stream's.
        template <typename Op>
        void abandon(Op& op) noexcept {
            if (opened) {
                opened = false;
                op.owner->release(op.index);
            }
        }
    };

    /// `accept(port)`: the listener's backlog first, else parks on the listener.
    struct AcceptPolicy {
        template <typename Stack>
        using ResultFor            = std::expected<Stream<Stack>, stream::Error>;
        static constexpr auto kind = WaitKind::Accept;

        std::uint16_t port = 0;

        template <typename Op>
        [[nodiscard]] std::optional<WaitOutcome> begin(Op& op) const noexcept {
            op.index = port;
            if (const auto listening = op.owner->ensure_listener(port); !listening) {
                return WaitOutcome{.error = listening.error()};
            }
            if (const auto ready = op.owner->take_backlog(port)) {
                return WaitOutcome{.value = *ready};
            }
            return std::nullopt;
        }

        template <typename Op>
        [[nodiscard]] static bool resume(Op&) noexcept {
            return true;
        }

        /// A connection taken from the backlog but never handed over goes back to the brick.
        template <typename Op>
        void abandon(Op& op) noexcept {
            core::force_non_static(this);
            if (op.phase == StreamWait::Phase::Queued && !op.outcome.error && !op.outcome.stopped) {
                op.owner->release(static_cast<std::uint32_t>(op.outcome.value));
            }
        }
    };

}  // namespace aloe::runtime::detail
