# IP base: Ethernet, ARP, IPv4 and ICMP echo

Status: approved in the design session of 2026-10-04 ("the rest is good, carry on"), after two
rulings by the author: the datagram seam is a drained list per transport, and ARP is a per-shard
cache whose resolutions are events and whose seeding is a plain call. Spec 3 of the four that lead
to minimal TCP, and the first half of roadmap phase 1. Tracks GitHub issue #5 `[NET]`. Builds on
the device layer (PRs #2 and #3) and the shard runtime with its loop bricks (PR #7), all merged on
`main`. Everything under "Open questions" is left for the plan or for a later step.

## Goal

The first protocol brick: one object per shard between the device queue and the transports. It
answers ARP and ICMP echo, sorts incoming IPv4 datagrams into one list per transport for the
layer above to drain, and sends a transport's segment by routing it, finding the next hop's MAC,
building the two headers and handing the frame to the queue. The kernel does all of this under
every socket; Aloe left the kernel, so this is where the stack first talks to it, on the simplest
protocols, before TCP sits on top in #6.

## Done when

1. `ping` from Linux over a tap device is answered: the manual test configures the kernel end of a
   `net_tap` interface, sends an ICMP echo request through the kernel's own stack, and receives
   the reply. The kernel's ARP and ICMP do the talking.
2. The same passes on the fabric against a harness port, with the port opened with
   `EmulatedOffloads::None` and with `EmulatedOffloads::Checksums`, so both the software and the
   offload path of every checksum run in CI.
3. ARP resolves an on-subnet peer and the gateway, a reply to our request appears in the
   resolution list, an unsolicited reply is ignored, and the cache ages entries out: a stale entry
   is refreshed in the background while still in use, an expired one is unresolved again.
4. A transport's segment reaches the wire through `send` over both backends, with the IPv4 and L4
   checksums filled by the device when it offers it and in software when it does not.
5. Tests still need no root, no hugepages and no network card; `debug`, `gcc-debug`, `asan` and
   `tsan` pass with the format check. The tap test and the example are local gates.
6. `docs/architecture/net.md` exists, the overview, roadmap, README and AGENTS.md list the module.

## Context

What exists and is relied on, verified in the code on 2026-10-04:

- `aloe::device`: `IsPacket` with `prepend`, `append`, `trim_front`, `trim_back`, `rx()`, `tx()`,
  `set_tx()`; `packet_headroom` is 128 bytes on every backend, and Ethernet plus IPv4 is 34.
  `Capabilities` reports the four checksum offloads. `RxMetadata` carries the device's verdicts,
  `TxMetadata` the header lengths and the fills wanted. `address.hpp` has `MacAddress` and
  `Ipv4Address`, `checksum.hpp` has `internet_checksum`, `ipv4_header_checksum`,
  `ipv4_pseudo_header_sum` and `ipv4_l4_checksum`, `protocol.hpp` has `Ipv4Protocol` with `Icmp`,
  `Tcp`, `Udp`, and `device.hpp` has `ethernet_header_size`.
- `aloe::loop::ShardQueue<Device>`: `allocate()`, `transmit(Packet&&)` which moves the packet only
  on success and returns false when the ring and the device are both full, `flush()`, and the
  device's facts. Nothing in it blocks, throws or allocates after construction.
- `aloe::fabric::Port` with `EmulatedOffloads::Checksums` fills checksums on transmit under the
  DPDK convention and asserts it: zero in the IPv4 checksum field, the pseudo-header sum in the L4
  field. On receive it sets `l3` and `l4` verdicts; with `None` every verdict stays `Unknown`. A
  frame that is not IPv4 is delivered to queue 0.
- `aloe::ethdev::Packet::set_tx` maps the fills to `RTE_MBUF_F_TX_IPV4`, `RTE_MBUF_F_TX_IP_CKSUM`
  and the TCP or UDP checksum flag and clears only those bits.
- `aloe::runtime::ShardContext::now()` is the tick stamp, so a runtime stack can pass it to the
  brick without a clock read.
- `tests/shared/device/frames.hpp` builds Ethernet and IPv4 frames with four checksum modes;
  `tests/shared/dpdk/packet_socket.hpp` is a raw socket on the kernel end of a tap;
  `aloe::testing::EalEnvironment` starts the EAL once per test binary.

## Decisions

Made in the design session of 2026-10-04, with the alternatives that lost.

| Decision                         | Chosen                                                                                                                                                                                                                                                                                      | Rejected                                                                                                                                                                                                                                                                                                                                                                                      |
|----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| The seam above IPv4              | `process(burst, now)` consumes the whole burst and fills one list of datagrams per transport; the caller drains `received(Tcp)` and `received(Udp)` and feeds each transport brick. A transport is templated on the IP type below it and transmits through it. The author's ruling.          | A top-down chain where the transport's `process` takes the raw burst and asks IP per packet: two transports on one core parse every frame twice. IP templated on its readers, or a handler table: the rejected callback direction. Annotating the burst in place: every transport scans all slots. One monolithic stack class: no module boundary, no two-core shard. |
| Who sees ARP                     | Nobody above IP. ARP is private to the brick; three things leak for startup and the multi-shard case: `send` fails with `Unresolved`, `resolved()` lists what was learned, `learn` and `resolve` seed the cache.                                                                            | ARP as a sibling brick the loop wires like a transport.                                                                                                                                                                                                                                                                                                                                       |
| ARP across shards                | A per-shard cache. Every mapping learned from an ARP frame is an event; the loop that owns queue 0 forwards it to the other shards, which call `learn`. The forwarding itself lands in #6 next to connection placement. The author's ruling.                                                   | Passive learning from the source MAC of IPv4 frames: spoofable, no kernel does it, helps neither outbound connects nor the gateway. One shared cache under a seqlock: shared state on the data path.                                                                                                                                                                                           |
| Unresolved next hop              | `send` fails with `Unresolved`, one request per entry per `arp_request_interval` at most, the caller keeps the packet and retries. TCP's SYN and retransmit absorb it; a gateway pre-resolves at startup.                                                                                     | Holding packets per pending entry, as the kernel does: a queue, its limit and its aging for a case the target pre-resolves anyway.                                                                                                                                                                                                                                                             |
| The ARP table                    | A custom fixed table: open addressing, linear probing bounded to eight slots, a power-of-two capacity, no allocation. A `TODO` asks to research replacing it with abseil's `flat_hash_set` once the connection table in #6 decides whether abseil enters at all. The author's ruling.          | Pulling abseil in for this table alone; `std::unordered_map`, which allocates per insert.                                                                                                                                                                                                                                                                                                      |
| Aging                            | Lazy, against the stamp the caller passes: reachable, then stale with a background refresh, then expired. No timer, no wheel; the brick takes the queue and the config and nothing else.                                                                                                     | A sweep timer in the wheel: a timer node and a wheel dependency for a table that changes a few times an hour.                                                                                                                                                                                                                                                                                  |
| Echo reply path                  | The reply is built in the request's packet and goes back to the frame's source MAC. The ARP cache is never consulted, so `ping` works on any shard before cross-shard ARP exists.                                                                                                             | Routing the reply through the cache, which another shard may have filled.                                                                                                                                                                                                                                                                                                                     |
| L4 checksums                     | `send` fills them when asked: the pseudo-header sum into the protocol's field and the device finishes, or software over the segment. A transport brick never computes a checksum.                                                                                                             | Each transport computing its own and knowing the offload convention.                                                                                                                                                                                                                                                                                                                          |
| The burst after `process`        | Every slot is empty: moved into a list, transmitted as a reply, or released. The runtime's leftover sweep finds nothing; a hand-written loop resets nothing.                                                                                                                                  | Leaving dropped packets for the caller, as the echo stack does today.                                                                                                                                                                                                                                                                                                                          |
| Module                           | One module `common/net/`, namespace `aloe::net`: ARP needs the IP configuration, ICMP is IP's control protocol, Ethernet is the dispatcher. TCP and UDP become their own modules.                                                                                                             | One module per protocol: four modules that cannot be used apart.                                                                                                                                                                                                                                                                                                                              |

## Module layout

`common/net/`, target `Aloe.Common.Net` with alias `Aloe::Common::Net`, umbrella `<aloe/net>`.
Links `Aloe::Common::Loop`, `Aloe::Common::Device` and `Aloe::Common::Utils`, never `core`, so
stdexec stays out by construction. The protocol headers are header-only; the ARP cache has a
`.cpp`. Header basenames stay unique across the repository.

| Header            | Holds                                                                                                                                                 |
|-------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ethernet.hpp`    | `EtherType`, `EthernetHeader`, `parse_ethernet`, `write_ethernet`, `multicast_mac`.                                                                   |
| `arp.hpp`         | `ArpOperation`, `ArpPacket`, `arp_packet_size`, `parse_arp`, `write_arp`.                                                                             |
| `ipv4_header.hpp` | `Ipv4Header`, `ipv4_header_size`, `parse_ipv4`, `write_ipv4`.                                                                                         |
| `icmp.hpp`        | `IcmpType`, `IcmpHeader`, `icmp_header_size`, `parse_icmp`, `write_icmp`.                                                                             |
| `arp_cache.hpp`   | `ArpCacheConfig`, `ArpCache`.                                                                                                                         |
| `datagram.hpp`    | `Datagram<Packet>`, `ArpResolution`.                                                                                                                  |
| `ipv4.hpp`        | `Ipv4Config`, `SendRequest`, `SendError`, `Ipv4Counters`, `Ipv4<Device>`.                                                                             |

CMake follows `common/loop/CMakeLists.txt`: one interface library per header group (`Net.Wire`
for the four protocol headers, `Net.Arp` for the cache, `Net.Stack` for the brick), folded by
`add_combined_library` into `Aloe.Common.Net`.

## Wire formats

Every protocol header is a plain struct with a parse and a write function over byte spans, all
`constexpr` and `noexcept`, in `aloe::net`. Parsing returns `std::nullopt` for anything the brick
would not act on, so the brick's receive path is one `if` per layer. Writing asserts the span is
long enough in debug builds. Tests use these functions directly, and TCP's phase 2 pre-built
headers will use `write_ipv4` and `write_ethernet` on a cached header.

```cpp
enum class EtherType : std::uint16_t { Ipv4 = 0x0800, Arp = 0x0806 };

struct EthernetHeader {
    device::MacAddress destination;
    device::MacAddress source;
    std::uint16_t ethertype = 0;
};
std::optional<EthernetHeader> parse_ethernet(std::span<const std::byte> frame);   // nullopt under 14 bytes
void write_ethernet(std::span<std::byte> out, const EthernetHeader& header);
device::MacAddress multicast_mac(device::Ipv4Address group);                      // 01:00:5e and the low 23 bits

enum class ArpOperation : std::uint16_t { Request = 1, Reply = 2 };

struct ArpPacket {
    ArpOperation operation;
    device::MacAddress sender_mac;
    device::Ipv4Address sender_ip;
    device::MacAddress target_mac;
    device::Ipv4Address target_ip;
};
inline constexpr std::size_t arp_packet_size = 28;
std::optional<ArpPacket> parse_arp(std::span<const std::byte> payload);   // Ethernet over IPv4 only: hardware
                                                                          // type 1, protocol 0x0800, lengths 6 and
                                                                          // 4, operation 1 or 2; else nullopt
void write_arp(std::span<std::byte> out, const ArpPacket& packet);

struct Ipv4Header {
    std::uint8_t header_length = 20;    // bytes, 20 to 60; options are accepted on receive and never written
    std::uint8_t dscp_ecn      = 0;
    std::uint16_t total_length = 0;
    std::uint16_t identification = 0;
    bool dont_fragment         = true;
    bool more_fragments        = false;
    std::uint16_t fragment_offset = 0;  // in units of eight bytes
    std::uint8_t ttl           = 64;
    device::Ipv4Protocol protocol{};
    std::uint16_t checksum     = 0;     // as found on parse; as given on write, zero for an offload fill
    device::Ipv4Address source;
    device::Ipv4Address destination;
    bool is_fragment() const;           // more_fragments or a non-zero offset
};
inline constexpr std::size_t ipv4_header_size = 20;
std::optional<Ipv4Header> parse_ipv4(std::span<const std::byte> payload);   // version 4, header length at least 20,
                                                                            // total length at least the header and at
                                                                            // most the span; else nullopt. Trailing
                                                                            // bytes past the total length are padding.
void write_ipv4(std::span<std::byte> out, const Ipv4Header& header);        // 20 bytes, options unsupported

enum class IcmpType : std::uint8_t { EchoReply = 0, EchoRequest = 8 };

struct IcmpHeader {
    std::uint8_t type = 0;
    std::uint8_t code = 0;
    std::uint16_t checksum = 0;
    std::uint32_t rest = 0;             // identifier and sequence number for an echo
};
inline constexpr std::size_t icmp_header_size = 8;
std::optional<IcmpHeader> parse_icmp(std::span<const std::byte> message);   // nullopt under 8 bytes
void write_icmp(std::span<std::byte> out, const IcmpHeader& header);
```

The ICMP checksum is `device::internet_checksum` over the whole message; it has no offload on any
card, so it is always software, both ways.

## The brick

```cpp
template <device::IsDevice Device>
class Ipv4 {
public:
    using Packet    = typename Device::Packet;
    using TimePoint = std::chrono::steady_clock::time_point;   // the loop's clock; a stamp the caller passes in

    Ipv4(loop::ShardQueue<Device>& queue, const Ipv4Config& config);   // validates and throws; transmits nothing

    void process(std::span<Packet> burst, TimePoint now) noexcept;                 // every slot is empty afterwards
    std::span<Datagram<Packet>> received(device::Ipv4Protocol protocol) noexcept;  // Tcp or Udp, asserted; until the next process
    std::span<const ArpResolution> resolved() const noexcept;                      // learned from ARP frames; until the next process

    std::optional<Packet> allocate() noexcept;                                     // data() empty, 34 bytes of headroom at least
    std::expected<void, SendError> send(Packet&& packet, const SendRequest& request, TimePoint now) noexcept;
    std::optional<device::MacAddress> resolve(device::Ipv4Address next_hop, TimePoint now) noexcept;
    void learn(device::Ipv4Address address, device::MacAddress mac, TimePoint now) noexcept;   // static entry or another shard's resolution; no event

    device::Ipv4Address address() const noexcept;
    std::uint8_t prefix() const noexcept;
    std::optional<device::Ipv4Address> gateway() const noexcept;
    std::uint16_t mtu() const noexcept;            // the device's
    std::uint16_t max_l4_size() const noexcept;    // mtu() - 20: what one send may carry; TCP's MSS source
    const Ipv4Counters& counters() const noexcept;
};

template <device::IsPacket Packet>
struct Datagram {
    Packet packet;                          // the whole frame; a transport moves it out to keep it
    device::Ipv4Address source;
    device::Ipv4Address destination;
    device::Ipv4Protocol protocol;
    std::uint8_t l3_offset;                 // 14 today
    std::uint8_t l3_length;                 // 20 to 60
    std::uint16_t l4_length;                // from the total length, never past the frame
    device::ChecksumVerdict l4_checksum;    // the device's verdict; the transport decides what Unknown means
    std::span<std::byte> l4() noexcept;     // packet.data().subspan(l3_offset + l3_length, l4_length)
};

struct ArpResolution {
    device::Ipv4Address address;
    device::MacAddress mac;
};

struct SendRequest {
    device::Ipv4Address destination;
    device::Ipv4Protocol protocol;
    device::L4Checksum checksum = device::L4Checksum::None;  // fill the L4 checksum field the caller left zero
};

enum class SendError : std::uint8_t { NoRoute, Unresolved, Oversized, Refused };
```

The brick is one object per shard. It is neither copyable nor movable, like `ShardQueue`. Every
member runs on the owning thread; nothing in it is synchronised, blocks, throws after construction,
logs, or allocates after construction.

### Configuration

```cpp
struct Ipv4Config {
    device::Ipv4Address address;
    std::uint8_t prefix = 24;
    std::optional<device::Ipv4Address> gateway;   // absent: an off-subnet destination is NoRoute
    std::uint8_t ttl = 64;
    std::size_t burst_capacity = 64;              // the most packets one process call takes; ethdev's burst
    std::size_t arp_capacity = 256;               // a power of two
    std::chrono::nanoseconds arp_reachable        = std::chrono::seconds{60};
    std::chrono::nanoseconds arp_expire           = std::chrono::seconds{120};
    std::chrono::nanoseconds arp_request_interval = std::chrono::seconds{1};
};
```

The constructor throws `std::invalid_argument` for: a zero address; a prefix above 32; a gateway
that is not on the subnet; a zero or non-power-of-two ARP capacity; a zero burst capacity; a
non-positive duration; `arp_reachable` not below `arp_expire`. Every shard gets the same config.
The identification counter starts at the queue index times 4096 so shards do not collide early;
with don't-fragment always set the field has no meaning, so this is tidiness only.

The two datagram lists and the resolution list are `std::vector`s reserved to `burst_capacity`
at construction and never grown; `process` asserts the burst is no larger in debug builds. The
runtime's `ShardConfig::receive_burst` and this capacity default to the same 64.

## Receive path

`process(burst, now)` first clears the three lists, which frees whatever the layers above did not
take from the previous tick. Then, per packet:

1. **Ethernet.** Shorter than 14 bytes: `dropped_short`. Destination neither our MAC nor broadcast:
   `dropped_not_for_us`. Ethertype neither ARP nor IPv4: `dropped_ethertype`.
2. **ARP** goes to the ARP handler below.
3. **IPv4.** `parse_ipv4` fails: `dropped_bad_header`. Header checksum: the device's `Good` is
   trusted, `Bad` is `dropped_bad_checksum`, `Unknown` means `ipv4_header_checksum` over the header
   must come out as zero. Destination must be our address, the limited broadcast or the subnet
   broadcast (undefined for a prefix above 30), else `dropped_not_for_us`. A fragment:
   `dropped_fragment`. Then by protocol: ICMP to the handler below; TCP and UDP fill a `Datagram`
   in their list and count `delivered_tcp` or `delivered_udp`; anything else is `dropped_protocol`.
4. A dropped packet is released in place. A reply is transmitted through the queue; a refusal
   counts `transmit_refused` and releases the packet.

After `process` every slot of the burst is empty. The datagram keeps the device's L4 verdict and
nothing else about the L4 checksum: the transport decides whether an `Unknown` verdict means a
software check, since UDP's zero checksum and TCP's rules differ.

## ARP

### Frames

Only Ethernet over IPv4 with lengths 6 and 4 is parsed; anything else is `dropped_arp_malformed`.
A frame whose sender IP address is our own is `dropped_arp_conflict`. Then:

- **A request for our address.** Learn the sender, then build the reply in the same packet:
  Ethernet destination the sender's MAC, source ours; operation `Reply`; sender fields ours,
  target fields the requester's. Transmit. Count `arp_requests_received` and `arp_replies_sent`.
- **A request for someone else.** Teaches nothing; released and counted as a request received.
- **A reply addressed to us** for an address with an entry in the cache, complete or not: learn
  the sender and count `arp_replies_received`. A reply for an address with no entry is
  `dropped_arp_unsolicited`, as the kernel does, so nothing on the segment can fill the cache.

Every mapping learned from an ARP frame, first resolution or refresh, appends one `ArpResolution`
to the list and counts `resolutions`. That is the feed the loop forwards to the other shards in
#6. `learn` called by the program appends nothing, so shards never echo each other.

### Cache

`ArpCache` is a fixed table, independent of the device, tested on its own with stamps.

```cpp
struct ArpCacheConfig {
    std::size_t capacity = 256;   // a power of two
    std::chrono::nanoseconds reachable, expire, request_interval;
};

class ArpCache {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Lookup {
        std::optional<device::MacAddress> mac;   // present when reachable or stale
        bool send_request = false;               // a request is due now; the stamp is recorded
    };

    explicit ArpCache(const ArpCacheConfig& config);   // validates and throws
    Lookup lookup(device::Ipv4Address address, TimePoint now) noexcept;
    void learn(device::Ipv4Address address, device::MacAddress mac, TimePoint now) noexcept;
    bool contains(device::Ipv4Address address) const noexcept;   // any entry, incomplete or expired included
    std::size_t size() const noexcept;
};
```

Each entry: address, MAC, incomplete or reachable, the stamp of the last confirmation, the stamp
of the last request. The table is open addressing with linear probing bounded to eight slots from
a multiplicative hash of the address. A slot is free or occupied and never goes back to free, so
probe chains stay intact without tombstones. A lookup probes until it finds the asked address,
whose age it then evaluates whatever it is, or reaches a free slot or the end of the window. An
insert first looks for the address, then takes the first free slot in the window, then the first
expired entry, and when there is neither evicts the entry with the oldest confirmation.

```cpp
// TODO: Issue#5 - research replacing this table with abseil's flat_hash_set once #6's connection
// table decides whether abseil enters the project: reserved capacity, inserts capped at it.
```

The policy, against the stamp the caller passes and with no timer:

| State of the entry                                        | `lookup` returns                                                                       |
|-----------------------------------------------------------|----------------------------------------------------------------------------------------|
| Reachable, confirmed less than `reachable` ago            | The MAC. No request.                                                                   |
| Reachable, confirmed between `reachable` and `expire` ago | The MAC, and `send_request` if the last request is more than `request_interval` old. A live flow never stalls on a refresh. |
| Confirmed more than `expire` ago, or incomplete           | No MAC, and `send_request` under the same rate limit.                                  |
| Absent                                                    | No MAC, an incomplete entry inserted, `send_request`.                                  |

`learn` sets the entry reachable with the confirmation stamp `now`, inserting it if absent.
`lookup` records the request stamp when it says to send, so the brick that acts on `send_request`
need not touch the table again.

### Requests

When `lookup` says to send, the brick allocates a packet, writes an ARP request with our MAC and
address as the sender and the asked address as the target, Ethernet destination the broadcast
address for an unresolved entry and the known MAC for a stale one, and transmits it. A refused
transmit counts `transmit_refused` and the request stamp stands, so the next attempt waits
`request_interval`; the alternative, re-sending every tick while the ring is full, is worse.
`resolve(next_hop, now)` is `lookup` plus that request, for a program that pre-resolves its router
at startup.

## ICMP echo

The ICMP checksum of every incoming message is verified in software; a bad one is
`dropped_bad_checksum`. An echo request whose IPv4 destination is our unicast address becomes the
reply in the same packet; a request to a broadcast address is `dropped_icmp`, as the kernel does by
default, and every other ICMP type is `dropped_icmp` too.

Building the reply: if the request carried IPv4 options, `trim_front` by their length so the reply
has a 20-byte header, since Aloe never sends options; the ICMP message stays where it is. Write the
Ethernet header with the frame's source MAC as destination and ours as source. Write the IPv4
header: total length over 20 plus the message, a fresh identification, don't-fragment, the
configured TTL, protocol ICMP, our address as source and the request's source as destination. Set
the ICMP type to `EchoReply`, zero the ICMP checksum field and recompute it over the message. Then
the IPv4 checksum by the same two paths `send` uses, and `set_tx`. Transmit. Count
`echo_replies_sent`.

The frame's source MAC is the last hop, and the last hop is the right next hop back whether the
pinger is on the subnet or behind a router, so the cache is never consulted.

## Transmit path

`send(packet, request, now)` takes a packet whose data is the L4 segment, from `allocate()` or any
packet with at least 34 bytes of headroom, asserted in debug builds. The packet is moved only on
success; on every error it is returned to the caller exactly as it was built.

1. **Route.** The limited broadcast and the subnet broadcast go to the broadcast MAC without ARP;
   a 224/4 destination to `multicast_mac`; an on-subnet destination is its own next hop; anything
   else goes to the gateway, or fails `NoRoute` without one.
2. **Next hop.** `ArpCache::lookup`. A MAC, or `Unresolved` after the request above is sent.
3. **Size.** The segment longer than `max_l4_size()`: `Oversized`.
4. **IPv4 header.** Twenty bytes prepended: total length, the identification counter, don't-fragment,
   the configured TTL, the request's protocol, our address and the destination. The checksum field
   is zero for now.
5. **L4 checksum**, when `request.checksum` is not `None`. The field is at offset 16 of the segment
   for TCP and 6 for UDP; the caller left it zero, asserted in debug builds. With
   `capabilities().tx_l4_checksum`: write `ipv4_pseudo_header_sum` into the field and set the fill
   in the metadata. Otherwise write `ipv4_l4_checksum` over the segment, with UDP's zero becoming
   0xffff.
6. **IPv4 checksum.** With `tx_ipv4_checksum`: leave zero and set the fill. Otherwise write
   `ipv4_header_checksum`.
7. **Ethernet header** prepended, then `set_tx` with the two header lengths and the fills chosen.
8. **Queue.** `queue.transmit`. A refusal trims the 34 bytes back off, clears the fills and returns
   `Refused`, so a retry prepends again. Success counts `datagrams_sent`.

Step 6, with step 5 when a fill is asked, is one function the echo reply shares with `send`, so
both IPv4 checksum paths are tested through `ping` on the fabric and not only through `send`.

## Counters

`Ipv4Counters`, one per brick, monotonic, read on the owning thread or after it has stopped, like
`loop::ShardCounters`, which keeps counting frames received and transmitted as it does now.

| Group     | Counters                                                                                                                                                                       |
|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Receive   | `frames`, `arp_requests_received`, `arp_replies_received`, `datagrams_received`, `delivered_tcp`, `delivered_udp`, `resolutions`                                               |
| Transmit  | `arp_requests_sent`, `arp_replies_sent`, `echo_replies_sent`, `datagrams_sent`, `transmit_refused` (a frame the brick itself built that the queue refused)                      |
| Drops     | `dropped_short`, `dropped_not_for_us`, `dropped_ethertype`, `dropped_arp_malformed`, `dropped_arp_conflict`, `dropped_arp_unsolicited`, `dropped_bad_header`, `dropped_bad_checksum`, `dropped_fragment`, `dropped_protocol`, `dropped_icmp` |
| Send      | `send_no_route`, `send_unresolved`, `send_oversized`, `send_refused`                                                                                                           |

## Error handling

`std::expected<void, SendError>` on the one hot-path operation that can fail for a reason the
caller acts on. Everything on receive is a drop with a counter, never an error to anyone. The
constructor throws `std::invalid_argument` on a bad config, the cold path. Nothing logs: the brick
has no logger, by the rule that `core` stays out of protocol modules.

## The runtime

No change. A program that uses the runtime writes a stack type constructed from `ShardContext&`,
`loop::ShardQueue<Device>&` and an `Ipv4Config`, holding an `Ipv4<Device>`, whose `on_receive`
calls `ip_.process(burst, context_->now())` and then whatever transports it has. The
per-tick hook and the flush before the tasks run that the roadmap lists for phase 1 arrive with
TCP's timers in #6, which is also where the loop that owns queue 0 starts forwarding resolutions
through the other shards' inboxes.

## Testing

All unit tests run on the fabric with no root, no hugepages and no network card. A shared fixture
in `tests/shared/net/net_fixture.hpp` holds a fabric, a harness port and a stack port, a
`ShardQueue` with its counters and an `Ipv4` over queue 0, parameterized over the two
`EmulatedOffloads`, with helpers to send a built frame from the harness and to receive what the
stack transmitted. `tests/shared/net/net_frames.hpp` builds ARP frames and ICMP echo frames over
the module's own write functions; the device frame builders stay as they are. Both are one
interface library, `Aloe.Tests.Shared.Net`, linking `Aloe::Common::Net` and `Aloe::Common::Fabric`.

`tests/unit_tests/common/net/`, one test binary `Aloe.Tests.Unit.Net`:

| File                   | Covers                                                                                                                                                                                                                                                                             |
|------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `test_net_wire.cpp`    | Round trips and malformed input for all four protocol headers: short spans, wrong version, header length under 20, total length past the span, padding accepted, ARP with a hardware type that is not Ethernet, `multicast_mac` against RFC 1112's examples.                        |
| `test_net_arp_cache.cpp` | With stamps and no device: miss inserts incomplete and asks for a request; learn resolves; reachable returns no request; stale returns the MAC and one request per interval; expired returns no MAC; eviction of the oldest in a full window; the probe bound; `contains`.          |
| `test_net_receive.cpp` | Both offload modes: a TCP and a UDP datagram land in their lists with the right fields and verdicts; the lists clear on the next `process`; every drop reason fires once and the burst is empty afterwards; `Unknown` verifies in software, `Good` is trusted, `Bad` drops; options accepted; a fragment dropped. |
| `test_net_arp.cpp`     | A request for us yields a reply with the right bytes on the harness side and an entry; a request for another host yields nothing; a reply to our request appends a resolution and resolves the entry; an unsolicited reply is dropped; our own address as sender is a conflict; `learn` appends no resolution; a stale entry refreshes with a unicast request while `send` keeps succeeding. |
| `test_net_icmp.cpp`    | Both offload modes: an echo request yields a reply with swapped addresses, our MAC as source, type 0, fresh identification, both checksums right; a request with options yields a 20-byte header; a bad ICMP checksum, a broadcast echo and another ICMP type are dropped; a pinger from off-subnet gets the reply to the frame's source MAC. |
| `test_net_send.cpp`    | Both offload modes: on-subnet, via the gateway after it resolves, limited and subnet broadcast, multicast; `NoRoute` without a gateway; `Unresolved` sends one request, a second send within the interval sends none, the reply makes the third succeed; `Oversized`; `Refused` on a full ring returns the packet unchanged; header bytes and either the pseudo-header sum plus fills or the full checksums, checked on the harness side; `max_l4_size`; a bad config throws. |

`tests/integration_tests/net/test_net_ring.cpp`, `Aloe.Tests.Integration.Net.Ring`: the brick over
an ethdev `net_ring` port, which loops a queue's transmit into its own receive. An ARP request and
an echo request injected through the port come back as a reply and are processed by the same
brick; a UDP segment sent through `send` comes back and lands in `received(Udp)` with its headers
and checksums intact. That exercises `prepend`, `trim_front` and `set_tx` on real mbufs with no
root, and is what done-when 4 means by "both backends".

`tests/manual_tests/net/test_net_tap.cpp`, `Aloe.Tests.Manual.Net.Tap`, root, run by hand like
the other tap tests: the EAL with `net_tap0,iface=aloe-ping`; the kernel end configured through
`ioctl` with 10.77.0.1/24 and brought up; the brick at 10.77.0.2/24 driven by a hand-written loop
on a `std::jthread` over `ShardQueue` and `Ipv4`, the loop the docs lead with; a raw ICMP socket
sends an echo request with a known identifier, sequence and payload to 10.77.0.2 and the test waits
for the matching reply. The kernel resolves the brick by ARP first, which is the issue's `ping`.

`examples/ping_responder/ping_responder.cpp`, `Aloe.Examples.PingResponder`: the same hand-written
loop on one queue of a DPDK port, `ping_responder <port> <address/prefix> [gateway] [-- EAL
arguments]`, printing the counters on interrupt. It is how a real card is checked by hand and what
`docs/architecture/net.md` shows.

## Documentation

- `docs/architecture/net.md`: what the module is, the key types, the hand-written loop over the
  brick, design notes: the seam, ARP private to IP, the reply path, aging without a timer, what
  the multi-shard case needs from #6.
- `docs/architecture/overview.md`: the loop sketch under "Two products" gains `ip.process` and
  `tcp.process(ip.received(...))`; the status lines under "Two products" and "Layers" name the IP
  base as existing; "Layers" names the datagram list as the seam between IP and the transports.
- `docs/roadmap.md`: phase 1's first paragraph marks Ethernet, ARP, IPv4 and ICMP echo as landed.
- `README.md` and `AGENTS.md`: a row for `aloe::net` in the module tables, the namespace in the
  rules, the module in the opening sentence of AGENTS.md.
- `docs/guides/getting-started.md`: the net tap test and the example beside the runtime tap test.

## Risks

- **Where ARP lands on a real card.** Both backends put non-IPv4 frames on queue 0; a card may
  use another default queue. The forwarding design in #6 reads the card rather than assuming.
- **Receive flags on a transmitted mbuf.** The echo reply reuses the request's mbuf, whose
  `ol_flags` still carry the receive bits. `set_tx` clears only the transmit bits. DPDK drivers
  ignore receive flags on transmit; the ring integration test is where a surprise would show.
- **The kernel end of the tap.** Configuring it through `ioctl` is more code than a packet socket
  but is what makes the kernel's own ARP and ICMP do the work the issue asks for. The test fails
  with a clear message when not root, like the runtime tap test.
  The tap test was compiled on 2026-10-05 and not run (no root), so the shared-MAC risk and the
  `SIOCSIFHWADDR` fallback stay unverified.
- **Options in a request.** The reply drops them by `trim_front`, which assumes the ICMP message
  need not move. It does not: it already sits after the options.

## Out of scope

Deferred, and written into the docs as such: forwarding resolutions between shards and flow rules
(#6); multicast reception and IGMP (the UDP step); ICMP errors such as destination unreachable
(phase 2, for TCP); IPv6 (the roadmap). Fragments are dropped and counted by the issue's decision
and never reassembled. The brick accepts no IPv4 options on transmit and ignores them on receive.

## Open questions

- Abseil: the `TODO` in `arp_cache.hpp`. Decided with the connection table in #6.
- Whether `received` gains a third list for protocols that are neither TCP nor UDP, for a program
  that speaks a raw protocol. Today they are dropped and counted.
- The class name `Ipv4` for a type that also holds ARP and ICMP. `Stack` would collide with the
  runtime's `IsStack` vocabulary in the docs; renaming is mechanical.
- `arp_reachable` at 60 seconds against the kernel's 30 with jitter. The refresh is unicast and
  does not stall anything, so the longer value costs nothing; revisit if a router ages faster.
- Whether the identification counter should start from a random value instead of the queue
  index times 4096.
- The plan's Task 1 placed the ARP target IP at offset 22, overlapping the target MAC; the
  implementation uses 24 (RFC 826) and the round-trip test pins it.

## Work order

For the plan, each step with its tests before the next starts:

1. Module skeleton, CMake targets, the umbrella, and the four wire headers with `test_net_wire.cpp`.
2. `ArpCache` with `test_net_arp_cache.cpp`.
3. `Ipv4Config` validation, `Datagram`, the receive path with the lists, the shared fixture and
   frame builders, `test_net_receive.cpp`.
4. ARP handling, the resolution list, `learn` and `resolve`, `test_net_arp.cpp`.
5. ICMP echo and the shared checksum function, `test_net_icmp.cpp`.
6. `send`, `allocate`, `max_l4_size`, `test_net_send.cpp`.
7. The ring integration test.
8. The manual tap test and the example.
9. `docs/architecture/net.md` and the other document updates.
10. Every preset and the format check; the final review.
