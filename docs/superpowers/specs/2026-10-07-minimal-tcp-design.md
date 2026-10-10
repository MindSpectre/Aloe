# Minimal TCP: handshake, in-order data, FIN and RST, the connection table, the stream concept

Status: design originally approved section by section in the sessions of 2026-10-06 and 2026-10-07, after
four rulings by the author: the data path copies nothing in either direction ("we fought against
the kernel not to add copying just cause"); the in-order receive chain is part of minimal TCP and
the other queues are phase 2; the scope boundary below; and the loop a program writes over the
bricks is what this step is measured by, the runtime second. This document is the written form,
for review before the plan. Spec 4 of the four that lead to minimal TCP, and the end of roadmap
phase 1 apart from UDP and the benchmark. Tracks GitHub issue #6 `[TCP]`. Builds on the IP base
(PR #9) and the refactoring that followed it (PR #10), both merged on `main`. Everything under
"Open questions" is left for the plan or for a later step. Revised after the latency review to
clarify the HFT scope, event draining, ACK ordering, byte-window accounting and table hashing;
this revision is for review before implementation planning.

## Goal

The simplest TCP that interoperates with Linux, and the stream contract every later layer reuses.
A brick, `tcp::Stack<Ip>`, one per shard over the IP brick and the shard's timer wheel, with the
four verbs every brick has: feed it the segments IP sorted out, drain its events, call its
non-blocking operations, flush. A connection is a zero-copy stream: the application decodes in
the received packets and encodes into the packets that leave. The runtime wraps the events in
senders afterwards, as the rule says, and gains the tick hooks it needs to do so.

Aloe primarily serves HFT order gateways and market-data handlers. The caller controls its
loop, core placement and transmit flushes through the bricks. A gateway may eventually favor
TX latency and a feed handler RX latency, placing maintenance on the less sensitive core.
That is a constraint on the ownership boundaries, not a two-core implementation in this phase.
The optional runtime serves applications that want scheduling and coroutines over the same bricks.

`send(bytes)` is a convenience over `prepare` and `commit`, not a promise of POSIX socket
buffering or a file-descriptor API. The in-place path remains primary. No unsent byte queue is
added to imitate `send(2)`, and no packet or core-placement choice is justified by a universal
small-message size threshold. The later benchmark measures the application's critical direction
under opposing traffic and maintenance work; the echoes here establish interoperability.

## Scope

In: the three-way handshake in both directions, in-order data, FIN, RST, a fixed window, the MSS
option; retransmission of SYN, SYN-ACK and FIN on a timer, because a connect would otherwise die
on its first ARP miss; a connection table keyed by 4-tuple with a stable index; inbound placement
by the card's RSS and outbound placement by the choice of local port; ARP resolutions forwarded
between shards; the stream concept as a module; the connection senders and the ready-made stack in
the runtime; the fixtures' TCP builder, parser and scripted peer.

Out, and listed again under "Out of scope": retransmission of data, out-of-order reassembly, SACK,
window scaling, timestamps, delayed ACK, zero-window probes, keepalive, TIME_WAIT and congestion
control (phase 2); flow rules and software steering between shards (fallbacks, deferred); UDP with
multicast and the tick-to-send benchmark (the rest of phase 1, separate issues).

Every connection runs on one thread in this phase, including receive, transmit and maintenance.
Multiple shards may run concurrently, each with its own connections. Actual two-core execution,
delegated transmission allowances, maintenance-core scheduling, asymmetric fences and
hardware-specific fast paths require later design and measurement.

## Done when

1. An echo server and an echo client written as a hand-written loop over the bricks, with no
   coroutine and no sender on the data path, interoperate with the Linux kernel stack over a tap
   device: a kernel socket connects to the server, gets its bytes back and sees a clean FIN
   exchange; the client connects to a kernel listening socket and does the same. A `manual` test.
2. The same two echoes written as runtime tasks over `runtime::TcpStack` do the same.
3. On the fabric, with no root: two bricks on two ports open, exchange data both ways and close;
   on a four-queue port with one brick per queue, every outbound connect lands its packets on the
   shard that opened it and every inbound SYN is taken by the shard the hash selects; a four-shard
   runtime serves connections on the owning shard under ThreadSanitizer.
4. `tcp::Connection` satisfies `stream::IsStream`, and `docs/architecture/stream.md` states the
   contract later layers build on.
5. Tests still need no root, no hugepages and no network card; `debug`, `gcc-debug`, `asan` and
   `tsan` pass with the format check. The tap tests and the examples are local gates.
6. `docs/architecture/tcp.md` and `stream.md` exist; the overview, roadmap, README and AGENTS.md
   list the modules.

## Context

What exists and is relied on, verified in the code on 2026-10-06:

- `aloe::net::Ipv4<Device>`: `process(burst, now)` sorts TCP datagrams into `received(Tcp)`, a
  `std::span<net::Datagram<Packet>>` valid until the next `process`. A `Datagram` holds the whole
  frame as a packet the transport moves out to keep, the addresses, `l3_offset`, `l3_length`,
  `l4_length`, the device's `l4_checksum` verdict, and `l4()`. `allocate()` gives a packet with 34
  bytes of headroom; `send(packet, {destination, Tcp, L4Checksum::Tcp}, now)` prepends both
  headers, fills both checksums by offload or software, and hands the packet back unchanged on
  `NoRoute`, `Unresolved`, `Oversized` or `Refused`. `resolved()` lists mappings learned from ARP
  frames; `learn` seeds the cache and reports nothing. `max_l4_size()` is the MTU less 20.
- `aloe::loop`: `ShardQueue<Device>` with `pending()`, `capacity()`, `index()`, `steering()`;
  `Timer` with `fire` of type `void (*)(Timer&) noexcept`, called with the timer unarmed so it may
  re-arm; `TimerWheel::arm`, `cancel`, `advance(now)`; `Work`, `RunQueue`, `Inbox`.
- `aloe::device`: `RssDescription` with `enabled`, `types.ipv4_tcp`, the key and the table;
  `flow_hash` and `queue_for` over a `FlowTuple`; `RxMetadata::rss_hash` carries the card's hash
  when present. The device concept has no flow-rule API.
- `aloe::wire`: `Ipv4Protocol::Tcp`, `ipv4_l4_checksum`, `load_be16`, `store_be32` and the rest;
  no `tcp/` directory yet; `fixtures.md` promises a scripted TCP peer in phase 1.
- `aloe::runtime`: `IsStack` requires `on_receive(burst)` only, called only on a non-empty burst;
  `Shard::step` is receive, `on_receive`, free leftovers, `run_once`, `flush`. The sender pattern
  to copy is `detail::TimerOperation`: an operation state that is a `loop::Work` and a
  `loop::Timer`, with a stop callback on the receiver's token. `ShardContext` has no view of its
  siblings.
- `aloe::fabric::Port` with more than one queue steers by `round_robin_rss` with every hash type;
  non-IPv4 frames land on queue 0; delivery is immediate and loss-free.
- `tests/manual_tests/net/test_net_tap.cpp` configures the kernel end of a tap through `ioctl`
  and drives a hand-written loop on a `std::jthread`: the template for the TCP tap tests.
- `codestyle.md` uses `aloe::net::tcp` as its namespace example; the module rule gives a module its
  own namespace, so the example changes to `aloe::tcp`.

## Decisions

Made in the design sessions, with the alternatives that lost.

| Decision | Chosen | Rejected |
|---|---|---|
| Receive contract | Zero-copy. A connection holds its in-order segments as packets on a chain; `unread()` is a range of `std::span<const std::byte>` over their unconsumed payloads; `consume(n)` releases packets as they empty. The author's ruling. | A per-connection byte buffer with one memcpy per segment and a contiguous `unread()`: a copy added for API convenience, which is what Aloe left the kernel to avoid. |
| Send contract | Zero-copy. `prepare(n)` hands out a span inside a fresh packet with header room reserved; `commit(n)` seals one segment and puts it in the transmit ring. `send(bytes)` is prepare, copy, commit for a caller that already holds bytes. Two commits are two segments, never one. | A send queue of bytes the stack segments later: a copy and a queue phase 1 does not need. |
| The receive chain | Intrinsic to minimal TCP: a window wider than one segment means segments wait for the application. One per-shard pool of nodes, each a packet with an offset, a length and a next pointer, chained per connection with a per-connection cap. The author's ruling. | A window of one segment, stop-and-wait, with `unread()` one span: phase 2 would rewrite the receive path and change the frozen contract. A fixed ring of slots per connection: reserves memory for every idle connection. |
| Window accounting | A fixed byte budget and a non-retreating advertised right edge; per-connection packet and per-shard node caps independently bound retained memory. Resource exhaustion drops unaccepted data without acknowledging it. | Free packet slots times MSS as the current byte window: a short segment consumes one slot but does not consume MSS bytes of sequence space. Byte accounting alone without memory caps. |
| ACK timing | Ordinary ACKs wait for `tcp.flush(now)` after application work, allowing a same-tick reply to carry them. Gaps, recovery and the full-segment ACK threshold have explicit earlier ACKs. Control segments are queued when generated. | One pure ACK for every ordinary small in-order segment. Unconditional coalescing of loss feedback. A delayed-ACK timer remains phase 2. |
| Control retransmit | SYN, SYN-ACK and FIN are re-sent from one timer per connection in the shard's wheel, doubling from one second, five tries, then a timeout. A SYN the IP brick could not send for want of a next hop is retried every 10 ms, not counted. Data is never retransmitted. | No retransmit at all: the first connect through an unresolved gateway dies. |
| TIME_WAIT | None: the last ACK takes a connection straight to CLOSED, as the roadmap puts TIME_WAIT in phase 2. A closed connection keeps its index entry until released and answers late segments with RST. | A TIME_WAIT state without its timer. |
| Events | An intrusive pending list, one entry and coalesced flags per connection. `poll_event()` removes one entry and returns its connection and a snapshot of its flags. Events survive processing and timers until drained or the connection is released. | Clearing events at the next receive pass. Calling application continuations from inside segment processing. |
| Connection table | A fixed open-addressing index from the tuple to a slot, linear probing, backward-shift deletion, no tombstones, no allocation after construction. Mix the RSS or software hash before selecting a local table bucket; RSS placement retains the original hash. No abseil; the `TODO` in `arp_cache.hpp` goes. | Directly masking the RSS hash again after queue selection. abseil's `flat_hash_map`, which rehashes and allocates, for a table that must be fixed. `std::unordered_map`. |
| Handles and lifetime | `Connection&` and `index()` are stable until the owner calls `release()`, the only thing that frees a slot. Release aborts first if the connection is open. | Freeing on close, which pulls unread data and the index from under the owner. A generation counter: unnecessary once release wakes every parked waiter. |
| Outbound placement | `connect` takes the first free ephemeral port whose `queue_for` is this queue. A card hashing addresses only fails `Unplaceable`. | Flow rules and software steering: the fallbacks, deferred with the device API they need. |
| ARP across shards | Every shard forwards what its `ip.resolved()` reports to every sibling, which calls `learn`. The runtime does it through `ShardContext::siblings()` and the inboxes; a hand-written program through its own channel. | Assuming ARP lands on queue 0. |
| Where the senders live | `common/runtime/stream/`, generic over the stream concept, plus `runtime::TcpStack<Device>`, a ready-made `IsStack`. The concept itself in a new module `common/stream/`, no stdexec, the way `device` holds the device concept. | Senders inside `tcp`: the brick would name stdexec. A concept in prose only: nothing pins that TLS satisfies it. |
| Errors in senders | `std::expected<T, stream::Error>` in the value channel; the stopped channel for cancellation only. Resolves the roadmap's open question. | The error channel: the P3552 task turns it into a throw at every `co_await`, for an outcome as ordinary as a peer's reset. |
| Deadlines | One timer per connection in the shard's wheel, owned by the runtime's slot; expiry is a cancel. Still one wheel per shard and never a timer per operation. | A stamp per connection swept by the tick: a pass over every connection each millisecond. |
| The tick hooks | Optional `on_tick(now)` drains events before tasks; optional `on_flush(now)` drains newly raised events and emits remaining ACKs after tasks. The queue flushes before and after tasks; ordinary pure ACKs are generated only in the latter phase. | Draining only from `on_receive`, which is not called on empty bursts. Emitting ordinary pure ACKs before the application can reply. |

## Module layout

Three new modules and additions to four.

| Module | Namespace | Target, umbrella | Holds |
|---|---|---|---|
| `common/wire/tcp/` | `aloe::wire` | part of `Aloe::Common::Wire` | `TcpSequence`, `TcpFlag`, `TcpFlags`, `TcpHeader`, `TcpOptions`. Values only. |
| `common/stream/` | `aloe::stream` | `Aloe::Common::Stream`, `<aloe/stream>` | `Event`, `Events`, `Error`, `IsReadView`, `IsStream`, `send(stream, bytes)`. Links `core` only. Header-only. |
| `common/tcp/` | `aloe::tcp` | `Aloe::Common::Tcp`, `<aloe/tcp>` | `TcpConfig`, `Endpoint`, `State`, `ConnectError`, `ListenError`, `ReadView`, `Connection`, `ConnectionEvent`, `Stack<Ip>`, `TcpCounters`, `FlowTable`. Links `net`, `stream`, `loop`, `wire`, `device`, `core`; never `execution` or `log`. |
| `common/net/` | | | `Ipv4<Device>` gains `using Device = Device;` and `queue()`, the `ShardQueue` it was built over, so a transport reaches `index()`, `steering()`, `pending()` and `capacity()` without a second constructor argument. |
| `common/runtime/` | | | `IsStack` with optional `on_tick` and `on_flush`; the new step order; `ShardContext::set_now(now)` and `siblings()`; `runtime/stream/`: `Streams<Stack>`, `Stream<Stack>`, the senders; `runtime/stack/`: `TcpStack<Device>`. Links `tcp` and `net` in addition. |
| `fixtures/frames/` | `aloe::frames` | | `TcpSpec`, `tcp_frame`, `ParsedFrame::tcp`, `TcpPeer`. |
| `tests/shared/tcp/` | `aloe::testing` | `...Shared.Tcp` | `TcpFixture`. |

Header basenames stay unique: `tcp_header.hpp`, `tcp_options.hpp`, `tcp_sequence.hpp` under wire;
`stream.hpp`, `stream_events.hpp` under stream; `tcp_config.hpp`, `tcp_connection.hpp`,
`tcp_stack.hpp`, `tcp_counters.hpp`, `flow_table.hpp`, `read_view.hpp` under tcp;
`streams.hpp`, `stream_handle.hpp`, `stream_senders.hpp`, `tcp_runtime_stack.hpp` under runtime.
The `FlowTable` and the node pool have `.cpp` files; the rest is templates.

## Wire formats

Every type is a plain struct with `parse` and `write` over byte spans, `constexpr` and `noexcept`,
like `Ipv4Header`. Nothing here holds state.

```cpp
struct TcpSequence {                      // RFC 793 arithmetic: comparison by signed difference
    std::uint32_t value = 0;
    constexpr TcpSequence operator+(std::uint32_t count) const noexcept;
    constexpr std::int32_t operator-(TcpSequence other) const noexcept;   // this - other, wrapped
    constexpr bool before(TcpSequence other) const noexcept;             // (*this - other) < 0
    constexpr bool after(TcpSequence other) const noexcept;
    friend constexpr bool operator==(TcpSequence, TcpSequence) = default;
};

enum class TcpFlag : std::uint8_t { Fin = 0x01, Syn = 0x02, Rst = 0x04, Psh = 0x08, Ack = 0x10, Urg = 0x20, Ece = 0x40, Cwr = 0x80 };

class TcpFlags {                          // a set of TcpFlag; raw() is the wire byte
public:
    constexpr TcpFlags() noexcept = default;
    constexpr TcpFlags(std::initializer_list<TcpFlag> flags) noexcept;
    [[nodiscard]] constexpr bool has(TcpFlag flag) const noexcept;
    [[nodiscard]] constexpr std::uint8_t raw() const noexcept;
    [[nodiscard]] static constexpr TcpFlags from_raw(std::uint8_t byte) noexcept;
};

struct TcpHeader {
    static constexpr std::size_t size            = 20;
    static constexpr std::size_t checksum_offset = 16;
    std::uint16_t source_port      = 0;
    std::uint16_t destination_port = 0;
    TcpSequence sequence{};
    TcpSequence acknowledgement{};
    std::uint8_t data_offset = 20;        // bytes, 20 to 60; options follow the fixed header
    TcpFlags flags{};
    std::uint16_t window         = 0;
    std::uint16_t checksum       = 0;     // as found on parse; zero on write, the IP brick fills it
    std::uint16_t urgent_pointer = 0;
    [[nodiscard]] static constexpr std::optional<TcpHeader> parse(std::span<const std::byte> segment) noexcept;
        // nullopt under 20 bytes, data offset under 20 or past the span
    constexpr void write(std::span<std::byte> out) const noexcept;   // 20 bytes; options are written separately
};

struct TcpOptions {
    std::optional<std::uint16_t> mss;     // kind 2; the only option read
    [[nodiscard]] static constexpr std::optional<TcpOptions> parse(std::span<const std::byte> options) noexcept;
        // NOP and EOL handled, every other kind skipped by its length; a length of 0 or 1, or one past
        // the span, is malformed: nullopt, and the brick drops the segment
    static constexpr std::size_t mss_size = 4;
    static constexpr void write_mss(std::span<std::byte> out, std::uint16_t mss) noexcept;   // kind 2, length 4
};
```

The TCP checksum is the IP brick's: `commit` leaves the field zero and asks for `L4Checksum::Tcp`.
On receive the brick trusts a `Good` verdict, drops `Bad`, and on `Unknown` checks
`wire::ipv4_l4_checksum` over the segment comes out zero.

## The stream concept

`common/stream/`, the vocabulary every layer shares. TLS in phase 3 and WebSocket in phase 4 hold
a connection of the layer below and offer the same members over their own buffers.

```cpp
namespace aloe::stream {

enum class Event : std::uint16_t {
    Connected = 1 << 0,   // an active open reached ESTABLISHED
    Accepted  = 1 << 1,   // a passive open reached ESTABLISHED; the application owns it from here
    Readable  = 1 << 2,   // bytes were added to unread()
    Writable  = 1 << 3,   // writable() increased, or a refused operation may be retried
    Acked     = 1 << 4,   // the peer acknowledged more of what was committed
    PeerClosed = 1 << 5,  // the peer's FIN arrived; unread() still holds what came before it
    Closed    = 1 << 6,   // both directions are done, or abort() was called
    Reset     = 1 << 7,   // the peer sent RST
    TimedOut  = 1 << 8,   // a control retransmit ran out of tries
};

class Events {            // a set of Event; one query per member, any(), raw()
public:
    [[nodiscard]] constexpr bool has(Event event) const noexcept;
    [[nodiscard]] constexpr bool connected() const noexcept;    // ... one per Event
    [[nodiscard]] constexpr bool any() const noexcept;
};

enum class Error : std::uint8_t { Reset, TimedOut, PeerClosed, Refused, Unplaceable, TableFull, NoPort, NoRoute, Closed };

template <typename V>
concept IsReadView = std::ranges::forward_range<V> &&
                     std::same_as<std::ranges::range_value_t<V>, std::span<const std::byte>> &&
                     requires(const V& view) {
                         { view.size() } -> std::same_as<std::size_t>;      // bytes, not chunks
                         { view.empty() } -> std::same_as<bool>;
                         { view.front() } -> std::same_as<std::span<const std::byte>>;
                     };

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

template <IsStream S>
std::size_t send(S& stream, std::span<const std::byte> bytes) noexcept;   // prepare, copy, commit per segment;
                                                                           // bytes accepted; never coalesces

}
```

The contract, in words, for `docs/architecture/stream.md`:

- **`unread()` is a view, not a copy.** The chunks are the layer's own storage: for TCP, the
  payloads of the received packets. A decoder reads in place and calls `consume` with what it
  used; a partial message stays where it is. The view is invalidated by `consume` and by the next
  `process`. A span over existing payload bytes remains usable until those bytes are consumed
  or the connection is released; appending later packets does not move their storage.
- **`prepare` and `commit` write in place.** `prepare(n)` gives at most `writable()` bytes of a
  fresh unit of transmission, a TCP segment, a TLS record. The encoder writes there once. `commit(k)`
  with `k` at most what was prepared seals and queues it; `commit(0)` discards it. One prepare is
  open at a time. A false `commit` means nothing was sent and the bytes are not accepted; the
  layer later raises a `Writable` retry hint; readiness is checked again at the retry.
- **Events are edges, state is level.** `events()` inspects undrained flags. The owning stack's
  `poll_event()` takes a snapshot and clears those flags before returning the connection to the
  caller. `process`, `flush` and timer advancement never clear undrained events. `unread()`,
  `writable()` and `peer_closed()` report current state; a waiter checks it before parking.
- **`send(bytes)` is convenience.** It copies into the prepared packet and reports the prefix
  successfully committed. The caller may reuse that prefix immediately; the unaccepted suffix
  remains the caller's. It adds no socket-style unsent queue, blocking behavior or guarantee of
  delivery. `prepare/commit` avoids that copy. Neither call waits for a runtime task to run.
- **`close` is half a close.** It sends the layer's end-of-stream after what was committed and
  refuses further sends; reading continues until the peer's end arrives, which raises `PeerClosed`.
  Both ends done raises `Closed`. `abort` tears down at once. `release` frees the slot, aborting
  first if needed, and is the owner's last call.
- **Nothing blocks, allocates, logs or names a clock.** Timing comes from the stamps the loop
  passes to `process` and `flush`.

## The brick

```cpp
namespace aloe::tcp {

struct Endpoint {
    wire::Ipv4Address address{};
    std::uint16_t port = 0;
};

enum class State : std::uint8_t { Closed, SynSent, SynReceived, Established, FinWait1, FinWait2, CloseWait, Closing, LastAck };
enum class ConnectError : std::uint8_t { TableFull, NoPort, Unplaceable, NoRoute };
enum class ListenError : std::uint8_t { InUse, TableFull };

class ReadView;   // models stream::IsReadView over a connection's chain; an iterator yields one span per node

class Connection {
public:
    using Sequence = wire::TcpSequence;

    [[nodiscard]] std::uint32_t index() const noexcept;        // stable until release
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] Endpoint local() const noexcept;
    [[nodiscard]] Endpoint remote() const noexcept;
    [[nodiscard]] stream::Events events() const noexcept;      // undrained flags; inspection does not clear them
    [[nodiscard]] std::uint16_t mss() const noexcept;          // what we send with: min(peer's option, ours)

    [[nodiscard]] ReadView unread() const noexcept;
    void consume(std::size_t count) noexcept;                  // count <= unread().size(), asserted
    [[nodiscard]] bool peer_closed() const noexcept;

    [[nodiscard]] std::size_t writable() const noexcept;       // min(mss, max(0, peer window - in flight)); 0 unless
                                                               // Established or CloseWait with no prepare open
    [[nodiscard]] std::optional<std::span<std::byte>> prepare(std::size_t count) noexcept;   // at most writable()
    [[nodiscard]] bool commit(std::size_t count) noexcept;     // seals and queues; false: not sent, bytes not accepted
    [[nodiscard]] std::size_t send(std::span<const std::byte> bytes) noexcept;   // stream::send(*this, bytes)
    [[nodiscard]] Sequence committed() const noexcept;         // the sequence after the last committed byte
    [[nodiscard]] Sequence acknowledged() const noexcept;      // snd_una: everything before it reached the peer
    [[nodiscard]] std::size_t unacknowledged() const noexcept; // committed() - acknowledged()

    void close() noexcept;    // FIN after what was committed; no more sends
    void abort() noexcept;    // RST now; Closed raised
    void release() noexcept;  // the slot is free; aborts first if not Closed
};

struct ConnectionEvent {
    Connection* connection;    // non-null; valid until release(), as for any connection handle
    stream::Events events;     // snapshot removed from the pending list by poll_event()
};

struct TcpConfig {
    std::size_t connections      = 1024;   // slots per shard
    std::size_t listeners        = 8;
    std::size_t receive_segments = 32;     // hard cap on held packets per connection; also sizes the initial byte budget
    std::size_t receive_pool     = 2048;   // shared retained-packet nodes; packet-pool sizing must reserve RX and TX capacity
    std::uint16_t ephemeral_first = 32768; // connect's local ports, inclusive
    std::uint16_t ephemeral_last  = 60999;
    core::Duration retry_initial  = std::chrono::seconds{1};        // SYN, SYN-ACK, FIN: doubles per try
    std::uint8_t retries          = 5;                              // then TimedOut
    core::Duration unresolved_retry = std::chrono::milliseconds{10}; // a SYN waiting for ARP; not a try
};

template <typename Ip>    // net::Ipv4<Device>; constrained on the members used
class Stack {
public:
    using Device = typename Ip::Device;
    using Packet = typename Ip::Packet;

    Stack(Ip& ip, loop::TimerWheel& wheel, const TcpConfig& config);   // validates and throws; allocates everything; sends nothing
    // neither copyable nor movable

    void process(std::span<net::Datagram<Packet>> segments, core::TimePoint now) noexcept;   // every packet moved out or released
    [[nodiscard]] std::optional<ConnectionEvent> poll_event() noexcept;  // takes and clears one pending notification
    void flush(core::TimePoint now) noexcept;               // pending ACKs and window updates as pure ACKs

    [[nodiscard]] std::expected<void, ListenError> listen(std::uint16_t port) noexcept;
    void unlisten(std::uint16_t port) noexcept;             // connections already open stay open
    [[nodiscard]] std::expected<Connection*, ConnectError> connect(Endpoint peer, core::TimePoint now) noexcept;
                                                            // never null on success; the SYN is in the ring
    [[nodiscard]] Connection& connection(std::uint32_t index) noexcept;   // the runtime's slots key on this
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::uint16_t mss() const noexcept;       // ours: max_l4_size() - 20
    [[nodiscard]] const TcpCounters& counters() const noexcept;
};

}
```

The stack keeps the stamp of the last `process`, `flush` or `connect` and uses it for everything a
connection does in between, so `prepare`, `commit`, `close` and the timer handler take no clock
and the stream concept names none. The ARP cache's timing is in seconds; a stamp one tick old
changes nothing.

### Configuration

The constructor throws `std::invalid_argument` for: zero `connections`, `listeners`,
`receive_segments` or `receive_pool`; an ephemeral range that is empty or contains port 0; a
non-positive duration; zero `retries`. Every shard gets the same config. At construction the stack
allocates the connections in a `std::deque<Connection>` (a `Connection` embeds a `loop::Timer` and
is immovable), the node pool as one `std::vector`, the flow table, the listener table and the free
lists, and never again. The per-shard random engine for initial sequence numbers is seeded from
`std::random_device` here, the one cold-path use of it.

### Storage per connection

Besides the receive block, the transmit block and the observations between them (below), a connection
holds its index, its two endpoints, the state, the event flags, the event-list links, the
pending-ACK link, the chain head, tail and count, the open prepare, the timer with its try
counter, and a pointer to its stack for the timer handler. Measure the final layout during
implementation; keep frequently used receive and transmit fields together. The node pool is
`receive_pool` nodes of a packet, a 16-bit offset, a 16-bit length and a next index: for the
ethdev packet, 16 bytes each, 32 KiB per shard at the default, independent of how many connections
exist.

## Receive path

`process(segments, now)` records the stamp and publishes deferred retry notifications, then
processes each datagram. It does not clear any event. Calling it with an empty span is valid and
advances the stamp and retry notifications without requiring incoming traffic.

1. **Header.** `TcpHeader::parse` on `l4()`; a failure is `dropped_bad_header`. Options between
   the fixed header and the data offset are parsed only on SYN and SYN-ACK; a malformed list is
   `dropped_bad_header` too.
2. **Checksum.** By the verdict: `Good` trusted, `Bad` is `dropped_bad_checksum`, `Unknown` is
   checked in software.
3. **Lookup.** The flow table by the received orientation: source address, source port,
   destination port. A hit goes to the receive half. A miss with SYN and no ACK to a listening port
   is a passive open. Any other miss is answered with RST, unless the segment is itself a RST, and
   counted `dropped_no_connection`; the reply's numbers follow RFC 793: with ACK, sequence equals
   the segment's acknowledgement number; without, sequence 0 and acknowledgement equals the
   segment's sequence plus its length, SYN and FIN counted.
4. **Disposition.** A held segment's packet is moved into a node. Every other packet is reset to
   an empty packet right there, so the pool gets it back now rather than at IP's next `process`.

### Passive open

A free slot is taken, the tuple inserted with the segment's hash, the state set to `SynReceived`,
the peer's sequence recorded as `rcv_nxt` after the SYN, the peer's MSS taken from the options or
536 without one, the peer's window from the header. An ISN is drawn. A SYN-ACK with our MSS
option leaves at once, the timer is armed. No slot free: `dropped_table_full`. Nobody owns the
connection yet: a RST, or the timer running out, frees the slot silently and counts
`handshakes_failed`. A duplicate SYN re-sends the SYN-ACK. The ACK completing the handshake, with
acknowledgement equal to our ISN plus one, moves it to `Established`, cancels the timer, and raises
`Accepted`: from that event on the application owns the slot.

### The receive half, per state

For `SynSent`: a SYN-ACK whose acknowledgement is our ISN plus one sets `snd_una`, `rcv_nxt`, the
peer's MSS and window, cancels the timer, moves to `Established`, marks an ACK pending and raises
`Connected`. A RST with that acknowledgement raises `Reset` and closes; the senders report it as
`Refused`. Anything else is dropped and counted `dropped_unexpected`.

For every synchronized state:

1. **Sequence check.** The segment's bytes, SYN and FIN included, are placed against
   advertised interval `[rcv_nxt, rcv_adv)`. A zero-length segment at `rcv_nxt` remains acceptable
   when the window is zero; a non-empty segment is not. Bytes before `rcv_nxt` are trimmed by advancing the node's
   offset, which costs nothing. A segment entirely before `rcv_nxt` is `dropped_duplicate`, one
   starting after it `dropped_out_of_order`, one reaching past the window `dropped_out_of_window`;
   each elicits an immediate ACK as described below, except that a rejected RST elicits no reply.
2. **RST.** `Reset` raised, state `Closed`, timer cancelled, held segments kept until release.
3. **ACK.** An acknowledgement in `(snd_una, snd_nxt]` advances `snd_una` and raises `Acked`. One
   past `snd_nxt` is answered with an ACK and dropped. The peer's window is updated by RFC 793's
   `wl1` and `wl2` rule; any increase in `writable()`, including an ACK freeing in-flight bytes,
   raises `Writable` so a threshold waiter can be retried. In `FinWait1` an
   acknowledgement covering our FIN moves to `FinWait2`; in `Closing` to `Closed` with `Closed`
   raised; in `LastAck` the same.
4. **Payload**, in `Established`, `FinWait1` and `FinWait2`. If the connection holds fewer than
   `receive_segments` and the pool has a node, the packet is chained, `rcv_nxt` advances,
   `Readable` is raised. Otherwise `dropped_no_slot` or `dropped_no_node`, with no advance of
   `rcv_nxt`, and an immediate ACK reports the last accepted byte. Recovery then depends on the
   peer's retransmission support. The ordinary and recovery ACK rules below apply to accepted data.
5. **FIN**, in order, meaning its sequence equals `rcv_nxt` after the payload: `rcv_nxt` advances
   by one, `PeerClosed` is raised, an ACK is pending. `Established` moves to `CloseWait`,
   `FinWait1` to `Closing`, `FinWait2` to `Closed` with `Closed` raised.

A segment for a connection in `Closed`, still indexed because not yet released, is answered with
RST and counted `dropped_closed`.

### The window

The byte budget `B` is fixed at construction: `min(receive_segments * mss(), 65535)`, calculated
without overflowing. With the default 32 segments and MSS 1460 it is 46720 bytes. Packet count
does not determine the remaining advertised window after each arrival. Separately,
`receive_segments` bounds retained packets per connection and `receive_pool` bounds their shared
nodes. These limits hold even when segments contain only one byte.

For the receive data phase, keep `unread_bytes`, `rcv_nxt` and `rcv_adv`, the right edge already
offered to the peer. On establishment the initial offer is `B` bytes beyond the peer's SYN.
Accepting `n` new bytes advances `rcv_nxt` and adds `n` to `unread_bytes`; consuming bytes reduces
`unread_bytes`. Before advertising a window, form the candidate edge
`rcv_nxt + (B - unread_bytes)`. Extend `rcv_adv` to that candidate only if it is later and the
connection has a free packet slot. Otherwise retain the existing edge. Shared-pool availability
is enforced on packet admission, not used as a second window-reopening condition: another
connection releasing a node must not be required to wake a window update. Compare sequence
numbers with `TcpSequence`, including across wrap. The wire window is `rcv_adv - rcv_nxt`, in
bytes; it never exceeds `B`. A refused transmit must not record
an offer that was not queued. A newly possible extension after `consume` marks an ACK pending,
including reopening a zero window. FIN processing does not extend the data window.

For example, receiving one byte at MSS 1460 reduces the wire window by one byte and leaves its
right edge unchanged, even though it uses a whole packet node. It does not retract 1460 bytes of
previously offered credit. This separates wire credit from memory accounting, following the
non-shrinking-window guidance in [RFC 9293, section 3.8.6](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.8.6).

A byte window cannot reserve enough packet nodes for every possible segmentation. Tiny segments
or other connections exhausting the shared pool can reach a memory cap while previously offered
byte credit remains. In that case drop unaccepted data without advancing `rcv_nxt` or
acknowledging it; do not force the wire window to zero by retracting credit. Byte credit remains
subject to the per-connection budget and is not a reservation against the shared pool. No copy
or unbounded allocation is introduced to hide exhaustion. The default pool is shared, not a reservation of
32 nodes for each of 1024 connections. Tests exercise these limits explicitly. Two minimal Aloe
peers cannot recover lost data until phase 2; this remains an interoperability prototype, not
an HFT deployment gate.

We send no window-scale option and do not use scaling. Device packet-pool sizing must cover RX
descriptors, retained RX packets, queued and NIC-owned TX packets, open prepares, pool caches and
headroom. Subtracting only RX descriptors from the pool size is not a sufficient sizing rule.

## Transmit path

`writable()` is `min(mss, max(0, peer window - (snd_nxt - snd_una)))` in `Established` or
`CloseWait` with no prepare open, else zero. The subtraction cannot underflow when a peer shrinks
its window. It does not consult the transmit ring or promise allocation success.

`prepare(count)` allocates from `ip.allocate()`, appends `min(count, writable())` bytes and returns
them. Nothing allocates when `writable()` is zero. A second `prepare` before `commit` is a
programming error, asserted.

Allocation failure also registers a deferred `Writable` retry hint. A refused prepare or commit
never spins inside the brick; the next `process`, including an empty one, raises at most one hint
per marked connection. The hint permits a retry, not a promise that the pool or NIC is available.

`commit(count)` trims the packet to `count`, prepends the 20-byte header with sequence `snd_nxt`,
acknowledgement `rcv_nxt`, flags ACK and PSH, our window, checksum zero, and calls
`ip.send(packet, {remote, Tcp, L4Checksum::Tcp}, stamp)`. Success advances `snd_nxt` by `count`,
clears the pending ACK since it rode along, counts `data_segments_sent`, and returns true.
`Refused` or `Unresolved` releases the packet, leaves every number as it was, counts
`send_refused` or `send_unresolved`, marks the connection for a `Writable` at the next `process`,
and returns false. `commit(0)` releases the packet and sends nothing. A commit is one segment: two
commits are two segments on the wire whatever their size, which is the "never coalesces" rule.

`send(bytes)` is `stream::send`: prepare, copy, commit, repeated while bytes remain and `prepare`
gives something; it returns the bytes accepted. A zero result can mean no progress, not just a
closed connection. `events()` and connection state describe terminal conditions. Acceptance means
ownership passed to the local transmit ring; it does not mean the frame has reached the wire.

`close()` in `Established` moves to `FinWait1`, in `CloseWait` to `LastAck`, and sends a FIN with
sequence `snd_nxt`, ACK set, then advances `snd_nxt` by one and arms the timer. In any other state
it does nothing. A refused FIN stays pending on the timer's first fire.

`abort()` sends a RST with sequence `snd_nxt`, cancels the timer, moves to `Closed`, raises
`Closed`. `release()` calls `abort` unless already `Closed`, returns the chain's nodes and their
packets, discards an open prepare, removes the tuple from the flow table, clears the flags, and
puts the slot on the free list. A released connection appears on no list.

### Events

Raising an event ORs its flag into the connection's pending flags and appends the connection to
the intrusive pending list only if it is not already present. `poll_event()` removes the head,
copies its flags into a `ConnectionEvent`, clears those flags and list membership, and returns
the snapshot. With no pending connection it returns `nullopt`. Removal happens before the caller
runs, so an event raised while handling the snapshot queues a fresh notification. The caller can
release the returned connection safely; its pointer is invalid from that release onward.
`release()` also removes any newly pending notification for that connection before reusing its
slot. Keep removal constant-time, with intrusive previous/next links or equivalent indices.

Processing a later burst, an empty burst, a timer firing or a transmit flush never consumes an
event. `Connection::events()` only inspects pending flags. Runtime waiters and hand-written loops
use the snapshot returned by `poll_event()`, not a second read of the cleared flags. No global
clear-at-tick operation exists. A timer event therefore survives even if another packet arrives
before the next event drain.

### ACKs

A pending ACK is a flag and a link on the stack's pending list. `flush(now)` walks the list and
sends one remaining ordinary pure ACK per connection, sequence `snd_nxt`, acknowledgement
`rcv_nxt`, our window. A refused pure ACK stays pending for the next flush. The pure ACK for a SYN-ACK is also deferred: a
client that commits data on its `Connected` event completes the handshake with that data segment.

Coalescing has exceptions. Each data segment above a gap, a fully duplicate data segment, an
out-of-window non-RST segment, or a resource-refused data segment attempts an ACK immediately
inside `process`. Remember the furthest end of data discarded above a gap; while recovering up
to that sequence, each segment advancing `rcv_nxt` also attempts an immediate ACK. This is only
a sequence marker, not out-of-order storage. Queue an ACK by the second full-sized in-order
segment since the last queued acknowledgement as well; saturate the count at two if sending is
refused, and reset it whenever a data or pure ACK carrying the current acknowledgement is queued
successfully. Thus ordinary short in-order
segments can share a same-tick ACK, but a burst of full-sized segments does not stretch the ACK
ratio indefinitely. These exceptions follow [RFC 5681, sections 3.2 and 4.2](https://www.rfc-editor.org/rfc/rfc5681.html#section-4.2).

An immediate ACK here means queueing it before processing the next segment; the caller still
controls the device flush. Three segments above a gap in one burst attempt three duplicate ACKs,
not one coalesced ACK at the end. If the queue refuses one, leave an ACK pending for a later flush;
never manufacture extra duplicate ACKs without corresponding received segments to replay a count.

### The timer

One `loop::Timer` per connection; `fire` casts back to the connection and calls the stack through
the connection's pointer. The deadline is `retry_initial` doubled per try. On fire: `SynSent`
re-sends SYN, `SynReceived` SYN-ACK, `FinWait1`, `Closing` and `LastAck` FIN. After `retries`
tries the connection moves to `Closed`, `TimedOut` is raised, and a `SynReceived` one that nobody
owns is freed. A SYN or SYN-ACK that `ip.send` refuses as `Unresolved` arms the timer at
`unresolved_retry` instead and does not count a try, so the first connect through a gateway whose
MAC arrives from another shard waits for the ARP round trip and little more.

### Ownership boundaries

Receive, transmit and maintenance are separate responsibilities with one owning thread per
connection in this phase. The blocks below describe logical state ownership, not a public ABI
or a commitment to separate cache lines:

```cpp
struct Receive {      // payload acceptance, consumption and acknowledgement state
    wire::TcpSequence rcv_nxt, rcv_adv;
    std::size_t unread_bytes;
    std::optional<wire::TcpSequence> recovery_end;
    std::uint16_t advertised_window;
    bool ack_pending;               bool peer_closed;
    std::uint8_t full_segments_since_ack;
    std::uint32_t chain_head, chain_tail, chain_count;
};
struct Transmit {     // preparation and outgoing sequence allocation
    wire::TcpSequence snd_nxt, iss;
    std::optional<Packet> prepared;
};
struct PeerFeedback { // accepted ACK and window observations used by transmit and maintenance
    wire::TcpSequence snd_una;       std::uint32_t snd_wnd;   std::uint16_t peer_mss;
    wire::TcpSequence wl1, wl2;
};
struct Maintenance { // setup, teardown and control retransmission
    State state;
    loop::Timer timer;
    std::uint8_t tries;
};
```

Receive processing updates receive state and validates peer feedback against transmitted
sequence state. Transmit consumes that feedback and the receive acknowledgement/window when
forming headers. A successfully queued piggyback ACK reports back to receive accounting through
one explicit operation, just as a pure ACK does. Maintenance handles state transitions and timer
arm/cancel requests from either direction. The owning stack coordinates these calls, the pending
event list and release. Document these dependencies rather than allowing unrelated helpers to
mutate all connection fields. `const` references express local read access; they do not establish
thread safety.

The future two-core design may put maintenance beside RX for a gateway or beside TX for a feed
handler. It must specify publication, feedback ordering, resource exhaustion, timer ownership
and connection teardown before concurrent use is supported. Replacing a record with a seqlock
does not by itself provide that contract; a snapshot can also lose ACK observations needed by
future recovery. No atomics, cross-core queues, delegated allowances or separate maintenance
thread are introduced for a connection in this phase. The runtime remains another caller of
the same single-threaded operations.

## The connection table

`FlowTable`, tested on its own: a fixed open-addressing table of `(key, index, mixed_hash)` entries, capacity
the next power of two at or above twice `connections`, linear probing, backward-shift deletion,
no tombstones. The key is the received orientation: remote address, remote port, local port,
twelve bytes with padding. The base hash is one function per stack, chosen at construction: when the
device's `steering()` is enabled with `types.ipv4_tcp`, the Toeplitz hash of the received tuple
with the device's key, which is what the card put in `rx().rss_hash`; a segment without the hash
gets it computed. Otherwise a multiplicative hash over the key. `connect` uses the base hash it
already computed while choosing the port. Every insert, lookup and backward-shift deletion uses
the same table-bucket rule: apply the 32-bit avalanche finalizer below, then mask with table
capacity minus one. Store the mixed hash with the entry so deletion can recover its home bucket.

```cpp
// Unsigned 32-bit arithmetic; a local detail helper, not an additional dependency.
hash ^= hash >> 16;
hash *= 0x85ebca6bU;
hash ^= hash >> 13;
hash *= 0xc2b2ae35U;
hash ^= hash >> 16;
```

Queue selection uses the original RSS hash, never this mixed value. RSS already conditions the
hashes reaching one shard: with a round-robin table and 16 queues, the low four bits are fixed
for that shard. Masking those same bits directly for a local power-of-two table would concentrate
home buckets. The extra mix redistributes the remaining bits; it does not change placement or
replace full-key equality checks. See [DPDK's predictable RSS description](https://doc.dpdk.org/guides/prog_guide/toeplitz_hash_lib.html#predictable-rss).

Free slots are a stack of indices. `listen` holds a small array of ports; a connect skips a
listening port while walking the ephemeral range.

## Placement

Inbound: nothing to do. The SYN lands on the queue the card's hash selects, that shard's listener
takes it, and every later segment of the tuple hashes the same way.

Outbound: `connect` walks the ephemeral range from a per-shard cursor, skipping listening ports and
tuples already in the table, and takes the first port for which
`device::queue_for(steering, {our address, peer address, port, peer port, Tcp})` with the tuple in
received orientation equals `ip.queue().index()`. With Q queues that is Q tries on average. When
`steering().enabled` is false, any free port does. When RSS is enabled but `types.ipv4_tcp` is
false and `types.ipv4` true, the queue depends on the addresses alone: computed once, and if it is
not ours `connect` fails `Unplaceable` rather than opening a connection whose packets would land
elsewhere. The cursor wrapping back to its start without a match is `NoPort`. A peer with no route
is `NoRoute`, checked by asking the IP brick before the slot is taken. The SYN leaves at once; an
`Unresolved` SYN is the timer's case above.

## ARP across shards

The IP base left it here. Every shard forwards each `ArpResolution` in `ip.resolved()` to every
sibling after `process`, and a sibling applies it with `ip.learn(address, mac, now)` on its own
thread, which reports nothing, so shards never echo each other. A hand-written multi-queue program
owns the channel; one `loop::Inbox` per loop and a `Work` node carrying the resolution is the
obvious one and what the docs show. The runtime: `ShardContext` gains `siblings()`, a span of the
other contexts' pointers that `Runtime` sets before `start`, and `TcpStack::on_receive` pushes one
`Work` per resolution per sibling through the inboxes, allocated on that cold path and freed on
arrival. Forward each `ip.resolved()` list once per `ip.process`, not repeatedly on empty ticks.

## Counters

`TcpCounters`, one per stack, monotonic, read on the owning thread or after it has stopped.

| Group | Counters |
|---|---|
| Segments | `segments_received`, `data_segments_sent`, `pure_acks_sent`, `control_segments_sent` (SYN, SYN-ACK, FIN), `resets_sent`, `retransmits`, `window_updates` |
| Connections | `connections_opened`, `connections_accepted`, `connections_closed`, `connections_reset`, `connections_timed_out`, `handshakes_failed` |
| Drops | `dropped_bad_header`, `dropped_bad_checksum`, `dropped_no_connection`, `dropped_closed`, `dropped_unexpected`, `dropped_duplicate`, `dropped_out_of_order`, `dropped_out_of_window`, `dropped_no_slot`, `dropped_no_node`, `dropped_table_full` |
| Send | `send_refused`, `send_unresolved` |

## Error handling

`std::expected` on `connect` and `listen`, the two cold operations that fail for a reason the
caller acts on; `bool` on `commit`; a count on `send`; drops with counters on receive; the
constructor throws on a bad config. Nothing logs, by the rule that keeps `log` out of the bricks.
Misuse, a second prepare, a consume past the view, a call on a released connection, is a debug
assertion.

## The runtime

### The tick

`IsStack` gains optional `on_tick(now)` and `on_flush(now)`, detected independently with
`requires` expressions; `EchoStack` changes nothing. `ShardContext::set_now(now)` records the
step stamp without running work or advancing timers. `Shard::step(now)` becomes:

1. Make the context current and call `context.set_now(now)` before any stack callback.
2. Receive the burst and call `stack.on_receive(burst)` when it is not empty; free leftovers.
3. Call optional `stack.on_tick(now)`: publish retry hints and drain pending connection events.
4. `queue.flush()`: control packets and immediate ACK exceptions already queued may leave now;
   ordinary pending ACKs have not yet been turned into packets.
5. `context.run_once(now)`: inbox, timers, then one snapshot of the ready chain, where woken tasks
   consume, prepare and commit. Work queued while that chain runs waits for the next step.
6. Call optional `stack.on_flush(now)`: drain events raised by timers and tasks, then emit any
   ordinary ACKs or window updates not already carried by application data.
7. `queue.flush()`: application data and remaining ACKs leave now.

Both hooks run on empty receive ticks as well. Events drained at step 6 enqueue work for the
next step; they do not recursively run another chain. Thus a timer-raised completion runs by
the next step even with no incoming traffic. A following receive pass cannot erase it. A
hand-written loop can instead advance its wheel before draining events to handle both sources
in one pass. The runtime does not wait to fill a receive burst, and the caller must keep each
application continuation short; this phase adds no preemptive scheduler or latency guarantee.

`ShardContext` gains `siblings()`; `Runtime` sets every context's view of the others after
constructing the shards and before `start`.

### The glue

`runtime::Streams<Stack>`, one per shard, constructed over a `tcp::Stack` and the context. Per
connection index it keeps one slot: a parked operation pointer for each of readable, writable,
acked, connected and closed, an `inplace_stop_source`, a `loop::Timer` for the deadline and the
threshold the readable or writable waiter asked for. Per listener it keeps the parked accept and an
intrusive list of accepted connections nobody has taken yet, populated when it drains `Accepted`.

`wake(now)`, called from both hooks, drains `tcp.poll_event()` and, per snapshot flag, pushes
the matching parked operation onto the run queue: one `loop::Work` push per operation, never a call into a task from
inside the pass. `Readable` checks the threshold against `unread().size()` first; `Writable` the
same against `writable()`. `Accepted` hands the connection to the parked accept or appends it to
the listener's list. `Reset`, `TimedOut` and `Closed` wake every parked operation on the
connection with the corresponding error. `cancel(c)` requests the slot's stop source: parked
operations complete stopped, the connection is aborted, and an operation started afterwards
completes stopped at once. `deadline(c, when)` arms the slot's timer; its fire is `cancel`.
`release` through the handle wakes every parked operation stopped before freeing the slot, so a
reused slot never meets a stale waiter.

Detach a parked operation from its slot before queueing its completion, so a second drain
cannot queue it twice. State is checked again when an operation starts; flags are only hints
to re-evaluate readiness. After a resource-refused prepare or commit, `send(bytes)` parks for
the next retry notification even if `writable()` remains positive, avoiding an inline retry loop.

### Senders

In `runtime/stream/stream_senders.hpp`, each a sender whose operation state is the wait node, in
the caller's frame, allocating nothing, completing on the shard. Every one registers a stop
callback on the receiver's token, so the scope's stop at shutdown unwinds a parked task as the
timer senders do today, and on the slot's own source.

| Sender | Completes with | When |
|---|---|---|
| `accept(port)` | `expected<Stream, Error>` | an accepted connection is available; `TableFull` if the listener failed |
| `connect(peer)` | `expected<Stream, Error>` | `Connected`; `Refused`, `TimedOut`, or a `ConnectError` mapped at once |
| `readable(n)` | `expected<std::size_t, Error>`, the unread bytes | `unread().size() >= n`; `PeerClosed` with fewer; `Reset`, `TimedOut` |
| `writable(n)` | `expected<std::size_t, Error>`, `writable()` | `writable() >= n`, `n` at most `mss()`; `Closed` if closed |
| `acked(sequence)` | `expected<void, Error>` | `acknowledged()` reaches it |
| `closed()` | `expected<void, Error>` | `Closed`; `Reset`, `TimedOut` |
| `send(bytes)` | `expected<std::size_t, Error>`, bytes accepted | all of `bytes` committed, parking on writable between segments |
| `close()` | `expected<void, Error>` | `close` then `closed()` |

One parked operation per kind per connection, asserted. Errors are in the value channel; the
stopped channel carries cancellation, deadlines and shutdown.

`runtime::Stream<Stack>` is a move-only owner of a connection: it exposes the stream members and
the senders above as members, and its destructor calls `release`, so a task that returns for any
reason, including unwinding on stop, frees the slot.

### The ready-made stack

`runtime::TcpStack<Device>` models `IsStack`: constructed from the context, the queue, an
`Ipv4Config` and a `TcpConfig`, it holds `net::Ipv4<Device>`, `tcp::Stack` and `Streams`.
`on_receive` is `ip.process` then `tcp.process` with the current context stamp, followed by
forwarding that pass's ARP resolutions once. `on_tick` calls `tcp.process({}, now)` to update
the stamp and retry hints even on an empty tick, then runs the wake pass. This empty pass is
safe after a non-empty one because processing does not consume events. `on_flush` runs the wake
pass again and then `tcp.flush(now)`. It exposes `ip()`, `tcp()` and `streams()`. The task
echo is `Runtime<ethdev::Port, runtime::TcpStack<ethdev::Port>>` with one task per connection. A
program composing its own stack writes its own `IsStack`, as the Ethernet echo does.

## The hand-written loop

What `docs/architecture/tcp.md` and the example lead with: server and client in one loop.

```cpp
using Port  = aloe::ethdev::Port;
using Ip    = aloe::net::Ipv4<Port>;
using Clock = aloe::core::Clock;

aloe::ethdev::Port port{{.name = "net_tap0", .queues = 1}};
aloe::loop::ShardCounters counters;
aloe::loop::ShardQueue<Port> queue{port, 0, 512, counters};
aloe::loop::TimerWheel wheel{std::chrono::milliseconds{1}, Clock::now()};
Ip ip{queue, {.address = {10, 78, 0, 2}, .prefix = 24, .gateway = {{10, 78, 0, 1}}}};
aloe::tcp::Stack<Ip> tcp{ip, wheel, {.connections = 1024, .receive_segments = 32}};
std::vector<Port::Packet> burst(64);

std::ignore = tcp.listen(7);                                                // the server half
auto session = tcp.connect({.address = {10, 78, 0, 1}, .port = 7}, Clock::now());   // the client half

for (;;) {
    const auto now             = Clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span{burst}.first(received), now);
    tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
    std::ignore = wheel.advance(now);  // timer events join receive events before the drain

    while (auto event = tcp.poll_event()) {
        aloe::tcp::Connection& c = *event->connection;
        const aloe::stream::Events events = event->events;
        if (events.connected()) {
            auto out = c.prepare(hello.size());
            std::ranges::copy(hello, out->begin());
            std::ignore = c.commit(out->size());
        }
        if (events.readable() || events.writable()) {
            while (!c.unread().empty()) {
                const std::span<const std::byte> chunk = c.unread().front();
                auto out = c.prepare(chunk.size());
                if (!out) break;                                            // resume on Writable
                std::ranges::copy(chunk.first(out->size()), out->begin());  // the echo's own copy
                if (!c.commit(out->size())) break;
                c.consume(out->size());
            }
        }
        if (events.peer_closed() && c.unread().empty()) c.close();
        if (events.closed() || events.reset() || events.timed_out()) c.release();
    }

    tcp.flush(now);
    std::ignore = queue.flush();
}
```

And the same echo as tasks:

```cpp
aloe::runtime::task<void> echo(aloe::runtime::Stream<Tcp> stream) {
    for (;;) {
        const auto readable = co_await stream.readable(1);
        if (!readable) break;
        while (!stream.unread().empty()) {
            const auto chunk = stream.unread().front();
            if (!co_await stream.send(chunk)) co_return;
            stream.consume(chunk.size());
        }
    }
    co_await stream.close();
}

aloe::runtime::task<void> serve(aloe::runtime::Streams<Tcp>& streams, aloe::runtime::Scheduler scheduler) {
    for (;;) {
        auto stream = co_await streams.accept(7);
        if (!stream) break;
        scheduler.spawn(echo(std::move(*stream)));
    }
}
```

## Testing

All unit tests run on the fabric with no root, no hugepages and no network card.

### Fixtures

`frames` gains `TcpSpec` (the addresses and MACs, ports, sequence and acknowledgement numbers,
flags, window, an optional MSS, the checksum mode), `tcp_frame(spec, payload)`, a `tcp` field in
`ParsedFrame`, and `TcpPeer`: a state holder on the harness side with `syn()`, `syn_ack(seen)`,
`ack(seen)`, `data(bytes)`, `fin()`, `rst()`, each returning the frame bytes with the right
numbers from what it has seen, and `see(frame)` to feed it what the stack transmitted. A test
reads like a packetdrill script, one tick at a time. `tests/shared/tcp/tcp_fixture.hpp` extends
the net fixture with a wheel, a `tcp::Stack`, a peer and helpers to tick, inject and collect.

### Unit tests

`tests/unit_tests/common/wire/test_wire_tcp.cpp`, in the wire binary: header round trip, data
offset under 20 and past the span, option parsing with NOP, EOL, MSS, an unknown kind skipped, a
zero length and a length past the span rejected, sequence arithmetic across the wrap.

`tests/unit_tests/common/stream/test_stream.cpp`: `tcp::Connection` satisfies `IsStream` and
`ReadView` satisfies `IsReadView`, as static assertions in a test that exists to hold them, and
`stream::send` over a stub stream splits at `writable()` and never merges two calls.

`tests/unit_tests/common/tcp/`, one binary `Aloe.Tests.Unit.Tcp`, every test parameterized over
both offload modes where the brick sends or receives:

| File | Covers |
|---|---|
| `test_tcp_table.cpp` | Insert, find and erase with backward shift; a full table; fixed vectors for the avalanche finalizer; lookup and deletion agree after mixing both hardware and software base hashes; hashes conditioned on 4 and 16 RSS queues do not retain those fixed low bits in their home buckets; the port cursor skips listeners and used tuples and wraps to `NoPort`. |
| `test_tcp_handshake.cpp` | Passive open: the SYN-ACK bytes, MSS option, a random ISN, window; the ACK raises `Accepted`; a duplicate SYN re-sends. Active open: the SYN bytes, the chosen port, `Connected`, the handshake ACK deferred and ridden by a same-tick commit. RST in `SynSent` is `Reset`; SYN-ACK and SYN retransmit at the doubling intervals; the unresolved 10 ms retry succeeds after the ARP reply; five tries then `TimedOut`; a full table drops the SYN; a SYN to no listener and a stray ACK are answered with RST. |
| `test_tcp_data.cpp` | Three segments give a view of three chunks; `consume` across a boundary releases the first packet; overlap is trimmed by offset; all three checksum verdicts; `prepare` clamped by MSS and the peer's usable window, including a shrunk window without unsigned underflow; two sends remain separate segments; `Writable` on any increase, including ACK progress and a threshold crossed from an already positive value; retry hints after allocation failure or refused commit even with empty input; `Acked` and `acknowledged()`; `commit(0)` sends nothing; a refused commit keeps every number. |
| `test_tcp_window.cpp` | The initial byte budget is 46720 for 32 slots at MSS 1460, capped at 65535 without overflow; a one-byte segment consumes one byte of advertised credit; the right edge stays fixed until consumption permits extension, including sequence wrap; consuming a partial packet changes byte accounting without freeing a node; byte-window exhaustion advertises zero and a consume reopens it; tiny segments can exhaust packet slots with nonzero remaining credit; global pool exhaustion across connections never exceeds either memory cap, never advances `rcv_nxt` for dropped bytes and never retracts the edge; a refused window update leaves the previous offer intact. |
| `test_tcp_acks.cpp` | Ordinary short in-order segments coalesce until flush; a same-tick reply carries their ACK without a following pure ACK; an ACK is queued by the second full-sized segment; three segments above a gap in one burst attempt three duplicate ACKs; duplicate and out-of-window data are ACKed immediately; rejected RSTs are not answered; recovery advances are ACKed immediately until the remembered gap end; queue refusal leaves an ACK pending without inventing duplicate-ACK counts. |
| `test_tcp_events.cpp` | Repeated flags coalesce per connection; `poll_event()` consumes exactly one snapshot; inspection does not consume it; events survive empty and non-empty `process` calls and flushes; a timer fires after a drain and its event survives the next receive pass; an application operation raises a fresh event after its prior snapshot was taken; releasing the current or another pending connection removes its entry and reuse never returns a stale pointer or flag. |
| `test_tcp_close.cpp` | Our close first, theirs first, simultaneous; `PeerClosed` with unread data still readable; `abort` sends RST; a received RST; FIN retransmit; `release` of an open connection sends RST and frees the slot; a segment to a closed, unreleased connection gets RST; a released connection raises nothing. |
| `test_tcp_two_stacks.cpp` | Two bricks on two fabric ports in one thread, the hand-written echo on one and a client loop on the other: connect, data both ways with sends larger than MSS, close, every counter accounted for. |
| `test_tcp_placement.cpp` | A four-queue fabric port with one brick per queue ticked round-robin, a peer port opposite. Every outbound connect chose a port whose `queue_for` is its queue and completed; every inbound SYN was taken by the hashed shard; `dropped_no_connection` is zero everywhere. `Unplaceable` through a wrapper device reporting an addresses-only RSS. |

`tests/unit_tests/common/runtime/test_streams.cpp`: `TcpStack` on a runtime over a fabric port, a
client brick on another port driven from the test thread. Each sender completes as the table says;
`cancel` and a deadline complete a parked readable stopped and abort the connection; `stop` drains
tasks parked on readable; `accept` keeps a backlog of three completed handshakes that arrived
before anyone waited. Both hooks together queue a parked operation at most once, and the wake
pass runs no task inline, pinned by a counter. A reply from a woken task carries the ordinary
ACK before the final flush; a reply that frees a zero window advertises the reopening in that
tick. A TCP timeout raised inside `run_once` completes its waiter on the next step with empty
input and also when that next step receives unrelated traffic. Callbacks see the supplied step
stamp, not the previous one. A refused send parks until a retry hint instead of spinning on
positive `writable()`. ARP forwarding occurs once per received resolution list. A stack with
neither optional hook keeps its existing behavior; hooks are independently optional.

`tests/unit_tests/common/runtime/threads/test_tcp_steering.cpp`, the one the `tsan` preset is for:
a four-shard runtime of `TcpStack` serving the task echo, a client runtime on a second port
opening connections to it, each reply stamped with the serving shard's index and matching
`queue_for`; and a connect from shard 3 through a gateway whose MAC only shard 0 learned.

### Integration

`tests/integration_tests/tcp/test_tcp_ring.cpp`, `Aloe.Tests.Integration.Tcp.Ring`: the brick
over `net_ring`, which loops a queue's transmit into its receive. A connect to our own address and
listening port completes as two connections in one table, and the echo runs over them through
real mbufs: headroom for 54 bytes, offsets, `set_tx`, no root.

### Manual

`tests/manual_tests/tcp/test_tcp_tap.cpp`, `Aloe.Tests.Manual.Tcp.Tap`, root, run by hand like
the net tap test: `net_tap0,iface=aloe-tcp`, the kernel at 10.78.0.1/24, the brick at 10.78.0.2.
Four tests: a kernel `SOCK_STREAM` socket connects to the hand-written echo server on port 7,
sends, receives its bytes, closes, and the test checks the connection ended with both FINs
acknowledged; the hand-written client connects to a kernel listening socket and does the same;
both again over `Runtime` and `TcpStack`. The kernel's SYN carries SACK-permitted, timestamps and
window scale, which the option skipper has to pass, and the tap offers no offloads, so every
checksum is software on our side and checked by the kernel.

### Examples

`examples/tcp_echo/tcp_echo.cpp`, `Aloe.Examples.TcpEcho`: the hand-written loop above on one
queue of a DPDK port, `tcp_echo <port> <address/prefix> [gateway] (--listen <port> | --connect
<host:port>) [-- EAL arguments]`, counters on interrupt. `examples/tcp_echo_tasks/`: the same over
`Runtime` and `TcpStack` on every queue of the port.

## Documentation

- `docs/architecture/stream.md`: the contract above, the loop, the convenience-only `send(bytes)`,
  explicit event consumption, what a layer must provide, and how the senders map onto it.
- `docs/architecture/tcp.md`: what the module is, the key types, the loop, design notes: zero-copy
  both ways, the chain and byte window with independent packet caps, ACK exceptions, ownership
  boundaries, the table's separate bucket hash and RSS placement,
  the comparison with Linux, Seastar and TLDK, what phase 1 leaves out and why.
- `docs/architecture/wire.md`: the `tcp/` row. `net.md`: `queue()`, the forwarding that now
  exists, the deferred list shortened. `fixtures.md`: the builder, the parser field, the peer.
  `runtime.md`: the tick with both hooks and its event-completion timing, `Streams`, the senders, `TcpStack`, errors in the value
  channel, deadlines as a timer per connection. `loop.md`: the TCP loop now exists.
- `docs/architecture/overview.md`: the loop sketch gets the real names and explicit event drain;
  the status lines name TCP and the senders as existing; "zero-copy view" and "deadlines" read
  as decided here. Replace the automatic seqlock/ring split promise with these ownership
  boundaries and the deferred concurrent design. Preserve the primary HFT use cases and the
  possibility of favoring either direction without requiring that composition in phase 1.
- `docs/roadmap.md`: phase 1 marks TCP landed with UDP and the benchmark remaining; the
  error-reporting and abseil questions resolved.
- `README.md`, `AGENTS.md`: rows for `stream` and `tcp`, the namespaces, the traps found.
  `codestyle.md`: the namespace example. `docs/guides/getting-started.md`: the tap tests and the
  examples.

## Risks

- **Kernel interop details.** The kernel sends options we ignore, may delay ACKs, and
  may combine data with FIN; all handled by the rules above, but the tap test is where a surprise
  shows. This machine has no root for it; the user runs it elsewhere, as with the net tap test.
- **Packet memory and advertised credit.** Retained packets, RX descriptors and outstanding TX
  compete for packet memory. Bound the retained nodes and provision the device pool for the RX
  and TX consumers described above. Tiny segments and a shared-pool shortage can force drops
  despite outstanding byte credit; tests must not mistake this for a guaranteed loss-free
  receive window. Data loss cannot be repaired between two minimal Aloe peers until phase 2.
- **Toeplitz orientation.** The table and the port choice must hash the received orientation with
  the device's key, or an outbound connection's own SYN-ACK misses the table. The placement test
  pins it on both backends' descriptions.
- **RSS-conditioned table hashes.** Hardware and software base hashes must share one bucket
  mixing rule, while queue prediction keeps the original RSS hash. Exercise lookup and deletion
  with many tuples that all belong to one shard, not only hashes uniform across all shards.
- **Events, timers and release.** Pending events are consumed only by `poll_event` or release,
  never by the next packet. Removal precedes delivery, so the handler may release the connection
  or cause a new notification. Two runtime wake passes must not enqueue one operation twice.
- **Threaded tests under tsan** take time; the steering test keeps connections in the tens.
- **A `std::deque<Connection>`** keeps immovable connections at stable addresses; the plan may
  choose `std::unique_ptr<Connection[]>` with a default constructor instead.

## Out of scope

Deferred, and written into the docs as such: retransmission of data, out-of-order reassembly,
SACK, window scaling, timestamps, delayed ACK, zero-window probes, keepalive, TIME_WAIT,
congestion control and pre-built headers (phase 2); flow rules per connection and software
steering between shards (fallbacks, with the device API they need); UDP with multicast and the
tick-to-send benchmark (the rest of phase 1); ICMP errors; IPv6; urgent data; SYN cookies; RFC
6528 initial sequence numbers; path MTU discovery. Nagle never: no coalescing is the design.

Also deferred: concurrent RX/TX halves for one connection; choosing a synchronization mechanism;
delegated transmission allowances; maintenance-core execution; asymmetric memory fences;
hardware-specific latency paths; and adaptive scheduling or burst tuning. The single-core
implementation records ownership dependencies without adding those mechanisms. POSIX socket
compatibility and a socket-style unsent byte queue are not goals implied by the convenient send
API. Full TCP will separately define payload retention through acknowledgement and NIC ownership
before adding data retransmission; this prototype's transmit ownership is not a recovery design.

## Open questions

- Whether `Connection` storage is a `std::deque` or a `std::unique_ptr<Connection[]>`.
- Whether `Events` is a class with named queries or an `enum class` with bitwise operators and a
  `has`; the spec writes the class.
- Whether `ReadView` is declared a `std::ranges::view` formally or only satisfies
  `forward_range`.
- The peer's MSS without an option: 536 per RFC 1122, as here.
- Whether the multi-queue hand-written forwarding of resolutions deserves a small helper in `loop`
  once the example shows it twice.
- The class name `Stack` for the brick against the runtime's `TcpStack`; both read well in their
  namespaces, and renaming is mechanical.

## Work order

For the plan, each step with its tests before the next starts. Establish the working brick and
its ownership contracts before adding runtime wrappers; the runtime is not a dependency of the
gateway loop. The latency review changes below belong to these existing deliverables, not a new
two-core implementation phase:

1. `wire/tcp/` with `test_wire_tcp.cpp`; `common/stream/` with `test_stream.cpp` over a stub.
2. `frames`: `TcpSpec`, `tcp_frame`, the parser field, `TcpPeer`.
3. `net`: the `Device` alias and `queue()`.
4. `tcp` skeleton: config validation, `FlowTable` with `test_tcp_table.cpp`, connection storage,
   the node pool, `listen`, the port choice.
5. The handshake both ways, the timer, RST replies: `test_tcp_handshake.cpp`, with the fixture.
6. Data: the chain, `unread` and `consume`, `prepare`, `commit`, `send`, byte-window accounting,
   ACK policy and explicit events: `test_tcp_data.cpp`, `test_tcp_window.cpp`, `test_tcp_acks.cpp`
   and `test_tcp_events.cpp`.
7. Close, abort, release: `test_tcp_close.cpp`.
8. `test_tcp_two_stacks.cpp` and `test_tcp_placement.cpp`.
9. Runtime: `set_now`, `on_tick`, `on_flush`, the step order, `siblings()`, `Streams`, the senders, `Stream`, `TcpStack`,
   `test_streams.cpp`, then the threads test.
10. The ring integration test.
11. The tap tests and the two examples.
12. The documents.
13. Every preset and the format check; the final review.
