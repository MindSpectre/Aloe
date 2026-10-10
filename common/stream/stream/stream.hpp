#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>

#include <stream_events.hpp>

namespace aloe::stream {

    /**
     * @brief A view over the unread bytes of a stream: a forward range of chunks, each a span into
     * the layer's own storage, with `size()` counting bytes, not chunks.
     */
    template <typename V>
    concept IsReadView =
        std::ranges::forward_range<V> && std::same_as<std::ranges::range_value_t<V>, std::span<const std::byte>> &&
        requires(const V& view) {
            { view.size() } -> std::same_as<std::size_t>;
            { view.empty() } -> std::same_as<bool>;
            { view.front() } -> std::same_as<std::span<const std::byte>>;
        };

    /**
     * @brief The zero-copy stream every layer offers: TCP now, TLS and WebSocket later.
     *
     * Reading is a view and `consume`; writing is `prepare` into the layer's next unit of
     * transmission and `commit`. Events are edges inspected through `events()` and drained by the
     * owning stack's `poll_event()`; `unread()`, `writable()` and `peer_closed()` are levels.
     */
    template <typename S>
    concept IsStream = requires(S& stream, const S& const_stream, std::size_t count) {
        { const_stream.index() } -> std::same_as<std::uint32_t>;
        { const_stream.events() } -> std::same_as<Events>;
        { const_stream.unread() } -> IsReadView;
        { stream.consume(count) } -> std::same_as<void>;
        { const_stream.peer_closed() } -> std::same_as<bool>;
        { const_stream.writable() } -> std::same_as<std::size_t>;
        { stream.prepare(count) } -> std::same_as<std::optional<std::span<std::byte>>>;
        { stream.commit(count) } -> std::same_as<bool>;
        { stream.close() } -> std::same_as<void>;
        { stream.abort() } -> std::same_as<void>;
        { stream.release() } -> std::same_as<void>;
    };

    /**
     * @brief Prepare, copy, commit, per segment, until `bytes` is accepted or the stream stops taking.
     *
     * Returns the prefix accepted; the caller keeps the suffix. Stops at the first null `prepare`
     * or false `commit`, never retries inline, and never merges two calls into one segment. A
     * convenience for a caller that already holds bytes: encoding into `prepare` avoids the copy.
     */
    template <IsStream S>
    std::size_t send(S& stream, const std::span<const std::byte> bytes) noexcept {
        std::size_t accepted = 0;
        while (accepted < bytes.size()) {
            const std::optional<std::span<std::byte>> room = stream.prepare(bytes.size() - accepted);
            if (!room) {
                break;
            }
            if (room->empty()) {
                std::ignore = stream.commit(0);  // nothing to write there: discard the preparation
                break;
            }
            const std::size_t count = room->size();
            std::ranges::copy(bytes.subspan(accepted, count), room->begin());
            if (!stream.commit(count)) {
                break;
            }
            accepted += count;
        }
        return accepted;
    }

}  // namespace aloe::stream
