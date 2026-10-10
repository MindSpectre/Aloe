#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace aloe::stream {

    /// What a stream reports; each is an edge, raised when the condition becomes true.
    enum class Event : std::uint16_t {
        Connected  = 1U << 0U,  ///< An active open reached ESTABLISHED.
        Accepted   = 1U << 1U,  ///< A passive open reached ESTABLISHED; the application owns it from here.
        Readable   = 1U << 2U,  ///< Bytes were added to `unread()`.
        Writable   = 1U << 3U,  ///< `writable()` increased, or a refused operation may be retried.
        Acked      = 1U << 4U,  ///< The peer acknowledged more of what was committed.
        PeerClosed = 1U << 5U,  ///< The peer's FIN arrived; `unread()` still holds what came before it.
        Closed     = 1U << 6U,  ///< Both directions are done, or `abort()` was called.
        Reset      = 1U << 7U,  ///< The peer sent RST.
        TimedOut   = 1U << 8U,  ///< A control retransmit ran out of tries.
    };

    /// A set of Event with one query per member.
    class Events {
    public:
        constexpr Events() noexcept = default;

        constexpr Events(const std::initializer_list<Event> events) noexcept {
            for (const Event event : events) {
                raw_ = static_cast<std::uint16_t>(raw_ | std::to_underlying(event));
            }
        }

        [[nodiscard]] static constexpr Events from_raw(const std::uint16_t raw) noexcept {
            Events events;
            events.raw_ = raw;
            return events;
        }

        [[nodiscard]] constexpr bool has(const Event event) const noexcept {
            return (raw_ & std::to_underlying(event)) != 0;
        }

        [[nodiscard]] constexpr bool connected() const noexcept {
            return has(Event::Connected);
        }
        [[nodiscard]] constexpr bool accepted() const noexcept {
            return has(Event::Accepted);
        }
        [[nodiscard]] constexpr bool readable() const noexcept {
            return has(Event::Readable);
        }
        [[nodiscard]] constexpr bool writable() const noexcept {
            return has(Event::Writable);
        }
        [[nodiscard]] constexpr bool acked() const noexcept {
            return has(Event::Acked);
        }
        [[nodiscard]] constexpr bool peer_closed() const noexcept {
            return has(Event::PeerClosed);
        }
        [[nodiscard]] constexpr bool closed() const noexcept {
            return has(Event::Closed);
        }
        [[nodiscard]] constexpr bool reset() const noexcept {
            return has(Event::Reset);
        }
        [[nodiscard]] constexpr bool timed_out() const noexcept {
            return has(Event::TimedOut);
        }

        [[nodiscard]] constexpr bool any() const noexcept {
            return raw_ != 0;
        }
        [[nodiscard]] constexpr std::uint16_t raw() const noexcept {
            return raw_;
        }

        constexpr Events& operator|=(const Event event) noexcept {
            raw_ = static_cast<std::uint16_t>(raw_ | std::to_underlying(event));
            return *this;
        }

        constexpr Events& operator|=(const Events events) noexcept {
            raw_ = static_cast<std::uint16_t>(raw_ | events.raw_);
            return *this;
        }

        friend constexpr bool operator==(const Events&, const Events&) noexcept = default;

    private:
        std::uint16_t raw_ = 0;
    };

    /// Why an operation on a stream did not succeed; the runtime's senders carry it in the value channel.
    enum class Error : std::uint8_t {
        Reset,        ///< The peer sent RST.
        TimedOut,     ///< A control retransmit ran out of tries.
        PeerClosed,   ///< The peer closed before the requested bytes arrived.
        Refused,      ///< An active open was reset, or a listener was in use.
        Unplaceable,  ///< No local port makes the card deliver the flow to this shard.
        TableFull,    ///< No free connection slot, or no free listener.
        NoPort,       ///< Every ephemeral port is taken or unplaceable.
        NoRoute,      ///< The peer is off the subnet and no gateway is configured.
        Closed,       ///< The connection is closed; the operation cannot complete.
    };

}  // namespace aloe::stream
