# Stream Module

The stream module (`common/stream/`) is the contract every stream-like connection models: TCP now, TLS
and WebSocket later. It holds no state and no code that runs a connection. It is the concepts a stream
satisfies, the event and error vocabulary, and one convenience helper that copies bytes through a stream.
Everything is reached through the umbrella `#include <aloe/stream>` (`export/aloe/stream`), and targets
link `Aloe::Common::Stream`. It depends on [`core`](core.md) only: no [`execution`](execution.md), no
[`log`](log.md), no DPDK.

## Key types

- **`aloe::stream::IsReadView<V>`** -- a forward range of `std::span<const std::byte>` chunks, each a span
  into the layer's own storage, with `size()` counting bytes (not chunks), `empty()` and `front()`. A read view is never a
  `std::ranges::sized_range`, whose `size()` is the chunk count: a view with a member `size()` opts out
  through `std::ranges::disable_sized_range`, and the concept rejects one that does not.
- **`aloe::stream::IsStream<S>`** -- the zero-copy stream: `index()`, `events()`, `unread()` (an
  `IsReadView`), `consume(n)`, `peer_closed()`, `writable()`, `prepare(n)`, `commit(n)`, `close()`,
  `abort()` and `release()`.
- **`aloe::stream::Event`** -- `Connected`, `Accepted`, `Readable`, `Writable`, `Acked`, `PeerClosed`,
  `Closed`, `Reset`, `TimedOut`; each an edge, raised when its condition becomes true.
- **`aloe::stream::Events`** -- a set of `Event` with one query per member (`connected()`, `readable()`,
  ...), `has`, `any`, `raw`, `from_raw`, `operator|=` and `==`.
- **`aloe::stream::Error`** -- why an operation did not succeed: `Reset`, `TimedOut`, `PeerClosed`,
  `Refused`, `Unplaceable`, `TableFull`, `NoPort`, `NoRoute`, `Closed`.
- **`aloe::stream::send(stream, bytes)`** -- prepare, copy, commit, per segment; returns the accepted prefix.

## Usage

A decoder reads the view and consumes what it used:

```cpp
template <aloe::stream::IsStream S>
void decode(S& stream) {
    const auto unread = stream.unread();
    std::size_t used  = 0;
    for (const std::span<const std::byte> chunk : unread) {
        used += parse(chunk);  // the decoder's own parser; returns the bytes it used
    }
    stream.consume(used);  // the view is invalid from here
}
```

An encoder writes in place and commits what it wrote:

```cpp
template <aloe::stream::IsStream S>
bool encode(S& stream, const Message& message) {
    const auto room = stream.prepare(message.encoded_size());
    if (!room) {
        return false;  // no room now; wait for Writable
    }
    const std::size_t written = message.encode_into(*room);
    return stream.commit(written);  // false: not accepted, the caller still owns the bytes
}
```

A caller that already holds the bytes uses `aloe::stream::send(stream, bytes)` and keeps the suffix the
return value does not cover.

## Design notes

- `unread()` is a view, invalidated by `consume` and by the next `process`. A chunk's bytes stay put until
  they are consumed or released.
- `prepare`/`commit` write in place, one prepare at a time; `prepare(n)` returns at most `n` bytes. A false `commit` means the bytes were not
  accepted and ownership of them stays with the caller; a `Writable` hint follows.
- Events are edges and state is level. `unread()`, `writable()` and `peer_closed()` are levels. Only
  `poll_event()` and `release()` clear flags.
- `send(bytes)` copies, reports the accepted prefix, never queues and never blocks. It stops at the first
  null `prepare` or false `commit`, never retries inline and never merges two calls into one segment. It is
  not POSIX `send(2)`.
- `close` is half a close, `abort` tears the connection down, and `release` is the owner's last call.
- Nothing in the contract blocks, allocates, logs or names a clock.

## In the runtime

The [runtime](runtime.md) wraps a TCP connection in `runtime::Stream<Stack>`, whose senders complete when the
event they wait for is raised, or at once when the level is already met. Every value is a
`std::expected<T, Error>`: an error is a value, not an exception. Cancellation, a deadline and the scope's
shutdown complete a sender through the stopped channel.

| Sender            | Value                         | Completes with                                                                 |
|-------------------|-------------------------------|--------------------------------------------------------------------------------|
| `readable(n)`     | `expected<size_t, Error>`     | the unread bytes once at least `n`; `PeerClosed` if the FIN came first with fewer; `Reset`, `TimedOut` |
| `writable(n)`     | `expected<size_t, Error>`     | the credit once `writable() >= n` (`n` at most the MSS); `Closed`, `Reset`, `TimedOut` |
| `acked(sequence)` | `expected<void, Error>`       | success once `acknowledged()` reaches `sequence`; the terminal error otherwise   |
| `closed()`        | `expected<void, Error>`       | success on a normal close; `Reset`, `TimedOut`                                  |
| `send(bytes)`     | `expected<size_t, Error>`     | every byte committed, parking between segments; the terminal error otherwise    |
| `close()`         | `expected<void, Error>`       | the brick's `close`, then as `closed()`                                         |
| `accept(port)`    | `expected<Stream, Error>`     | the next accepted connection, from the backlog first; `TableFull`, or `Refused` when the port is listened to outside the runtime |
| `connect(peer)`   | `expected<Stream, Error>`     | the connection once established; `Refused` on a reset before that; `TimedOut`, `TableFull`, `NoPort`, `Unplaceable`, `NoRoute` |

The handle's `send(bytes)` and `close()` are senders; the brick's synchronous ones are reached through
`connection()`, which models `IsStream`. The handle does not model `IsStream`, by design.
