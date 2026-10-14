# Net Module

The net module (`common/net/`) is the first protocol brick: IPv4 over one device queue, with the ARP and
ICMP echo that serve it. One object per shard sits between the [loop](loop.md)'s `ShardQueue` and the
transports. It answers ARP and ping, sorts incoming IPv4 datagrams into one list per transport for the
layer above to drain, and sends a transport's segment by routing it, finding the next hop's MAC, writing
both headers and handing the frame to the queue. Everything is reached through the umbrella
`#include <aloe/net>` (`export/aloe/net`), and targets link `Aloe::Common::Net`. It depends on
[`loop`](loop.md), [`device`](device.md), [`wire`](wire.md) and [`core`](core.md), and not on [`execution`](execution.md) or
[`log`](log.md): no header
here names the asynchronous model, logs, or reads a clock.

## Key types

- **`aloe::net::Ipv4<Device>`** -- the brick. Constructed from a `loop::ShardQueue<Device>&` and an
  `Ipv4Config`, which it validates. `process(burst, now)` consumes a burst: ARP and echo are answered,
  junk is dropped and counted, and the IPv4 datagrams for the transports are parsed into their lists. Every
  slot of the burst is empty afterwards. `received(protocol)`, for TCP or UDP, and `resolved()` are the
  lists, valid until the next `process`. `allocate()` gives a packet with room for both headers;
  `send(packet, request, now)` routes it, prepends the headers, fills the checksums and queues it, or
  returns a `SendError` and hands the packet back exactly as it was. `resolve(next_hop, now)` and
  `learn(address, mac, now)` are the two ways into the ARP cache from outside. Nothing here blocks,
  throws after construction, or allocates anything but packets after construction.
  **Transport access:** `Device` is the template parameter, `queue()` is the shard queue the brick was built
  over (its index, steering, ring and device), and `next_hop(destination)` answers the route question with no
  side effect: the destination on the subnet, the gateway off it, nothing with no gateway. `send` decides
  through the same call, and a transport asks it before committing resources, so a connect to an unroutable
  peer takes no slot. `Ipv4Config::accept_unsolicited_replies` (off by default, the RFC 826 merge rule) lets
  the shard whose queue receives a reply that a sibling shard solicited learn it and report it in `resolved()`
  for forwarding, since a multi-queue device's shards share one address; only a reply addressed to our
  address from a plausible sender is accepted.
- **`aloe::net::Datagram<Packet>`** -- one received datagram for a transport: the whole frame as a packet
  the transport moves out to keep, the two addresses, the protocol, the header offsets, the L4 length from
  the IPv4 total length, and the device's L4 checksum verdict. `l4()` is the segment.
- **`aloe::net::Ipv4Config`** -- the address and prefix, an optional gateway, the TTL, the burst capacity
  the lists are sized to, and the ARP capacity and timings. The same config goes to every shard.
- **`aloe::net::SendRequest`**, **`SendError`** -- the destination, the protocol and which L4 checksum to
  fill; `NoRoute`, `Unresolved`, `Oversized` or `Refused`.
- **`aloe::net::ArpResolution`** -- a mapping learned from an ARP frame, reported for the loop to forward
  to the other shards.
- **`aloe::net::ArpCache`** -- the per-shard table of IPv4 address to MAC, a fixed open-addressing table
  with bounded linear probing, aged lazily against the stamp the caller passes. Reachable, then stale with
  a background refresh, then expired.
- **`aloe::net::Ipv4Counters`** -- what happened, one counter per drop reason and per send error.
- **The wire formats** -- `EthernetHeader`, `ArpPacket`, `Ipv4Header`, `IcmpHeader` and the addresses
  are the [wire](wire.md) module's; the brick reads and writes them and holds none of its own.

## Usage

The hand-written loop that answers ARP and ping on one queue of a DPDK port, as
`examples/ping_responder/ping_responder.cpp` has it:

```cpp
#include <aloe/ethdev>
#include <aloe/loop>
#include <aloe/net>

aloe::ethdev::Port port{{.name = "net_tap0", .queues = 1}};
aloe::loop::ShardCounters counters;
aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 64, counters};
aloe::net::Ipv4<aloe::ethdev::Port> ip{queue, {.address = {10, 77, 0, 2}, .prefix = 24, .gateway = {{10, 77, 0, 1}}}};
std::vector<aloe::ethdev::Packet> burst(64);

std::ignore = ip.resolve(*ip.gateway(), std::chrono::steady_clock::now());  // ask for the router's MAC now
std::ignore = queue.flush();

while (running) {
    const auto now             = std::chrono::steady_clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span{burst}.first(received), now);   // ARP and ping answered in here; nothing else to do yet
    std::ignore = queue.flush();
}
```

With transports the same loop gains one line per transport and nothing else. The transports are templated on
the IP type below them and transmit through it; TCP exists (see the [tcp page](tcp.md)), UDP comes next:

```cpp
aloe::tcp::Stack<aloe::net::Ipv4<aloe::ethdev::Port>> tcp{ip, wheel, tcp_config};

ip.process(std::span{burst}.first(received), now);
tcp.process(ip.received(aloe::wire::Ipv4Protocol::Tcp), now);
// udp.process(ip.received(aloe::wire::Ipv4Protocol::Udp), now);   // once UDP lands
```

A transport sends by appending its segment to a packet from `allocate()`, with the checksum field zero,
and asking the brick to fill it:

```cpp
auto packet = ip.allocate();
// append the UDP header and payload to *packet ...
const auto sent = ip.send(std::move(*packet), {.destination = peer, .protocol = aloe::wire::Ipv4Protocol::Udp,
                                                .checksum = aloe::device::L4Checksum::Udp}, now);
if (!sent) {
    // sent.error(): NoRoute, Unresolved (a request is out: retry later), Oversized, Refused. The packet is yours again.
}
```

## Design notes

**The seam is a list, not a call.** IPv4 cannot be templated on who reads it, so it does not call anyone.
It sorts the datagrams it accepts into one list per transport, and the loop hands each list to its brick.
A core that runs TCP alone writes one line and pays nothing for the UDP list; a core that runs a feed and a
gateway writes two, in the order it wants. The dispatch is explicit code the program owns, which is what
the bricks promise, and the runtime writes those lines once.

**ARP is private to IP.** Nothing above the brick sees ARP, as nothing above a socket does. Three things
leak, all for startup and the multi-shard case: `send` fails `Unresolved` while a request is out and the
caller retries, which TCP's SYN and retransmit absorb; `resolved()` reports every mapping learned from the
wire; `learn` seeds the cache with a static entry or another shard's resolution and reports nothing, so
shards never echo each other. ARP frames carry no IP header, so a card puts them on its default queue,
queue 0 on both backends; the loop that owns that queue forwards resolutions to the other shards. A
request from another host for a third party refreshes an entry we already hold and never creates one, so a
router's gratuitous ARP after a failover takes effect at once; a sender that cannot be a host (a zero,
multicast or broadcast address) is answered when it asks for our address and never learned, which keeps
RFC 5227 probes working. A request from a group MAC is dropped as martian instead, since the reply would go
to a group.

**Aging without a timer.** The cache keeps the stamp of each entry's last confirmation and compares it
with the stamp the caller passes. Fresh entries are used as they are; stale ones are still used while one
unicast refresh goes out per interval, so a live flow never stalls on a refresh; expired ones are
unresolved again. No timer node, no wheel, and the brick's constructor takes the queue and the config and
nothing else. The table holds `arp_capacity` entries in windows of eight. A lookup miss or a learn in a
full window evicts, in this order: an expired entry; an incomplete one whose request is at least
`arp_request_interval` old, since its reply is not coming; the entry with the oldest confirmation; and only
then the incomplete entry whose request left first. An entry waiting for its reply goes last because
evicting it drops that reply as unsolicited, and two destinations that share a window would keep evicting
each other's requests. The price is that a storm of distinct unresolved destinations displaces live
entries, the gateway's included, and each displaced one costs one ARP round trip on its next send. The
capacity is a power of two of at least eight.

**The echo reply never consults the cache.** It is built in the request's packet and goes back to the
frame's source MAC: the last hop, which is the right next hop back whether the pinger is on the subnet or
behind a router. That is what makes `ping` work on any shard without cross-shard ARP.

**Checksums in one place.** `send` and the echo reply share one function: the IPv4 header checksum and,
when asked, the L4 checksum, filled by the device when `capabilities()` offers it and in software when
not. A transport never computes a checksum and never learns the offload convention. The fabric emulates
the offload, so both paths run in CI.

**Every slot empty.** `process` moves a packet into a list, transmits it as a reply, or releases it, so a
hand-written loop resets nothing and the runtime's leftover sweep finds nothing. Datagrams whose source is a
multicast, limited-broadcast or loopback address, or our own address, are dropped and counted as martian, as
the kernel does at IP input; TCP never sees them. Broadcasts reach UDP, and never TCP: RFC 1122 makes TCP
unicast only, so a TCP segment to the limited or subnet broadcast is dropped and counted here rather than
left to every transport to check.

**A config that cannot be a host is refused.** The address must be a unicast host on its subnet: not zero,
multicast, loopback or 255.255.255.255, and not the subnet's network or broadcast address when the prefix is
30 or less (a /31 or /32 has neither). The gateway must be another such host on the same subnet.

**Resolutions cross shards through the loop.** On a multi-queue device every shard has the same address, so
the gateway's reply to a request shard 2 sent arrives on queue 0, whose brick never asked. Every shard's
brick on such a device sets `accept_unsolicited_replies`, so queue 0 learns the reply and reports it in
`resolved()`, and the loop that owns queue 0 hands each resolution to every other shard, which calls
`learn(address, mac, now)` on its own thread. `learn` reports nothing, so a forwarded resolution is never
forwarded back. The runtime's `TcpStack` does all of this (see the [runtime page](runtime.md)); it sets the
flag itself when its queue's device has more than one queue. A hand-written multi-queue program sets the
flag in the `Ipv4Config` it gives every shard and owns the channel: one `loop::Inbox` per loop, and a
`loop::Work` node carrying the address and the MAC, pushed to every other loop's inbox after each
`process` that reported something and run there by a call to `learn`. Each `resolved()` list is forwarded
once, after the `process` that filled it, never again on a pass that received nothing.

**Deferred.** Flow rules come with later placement work; multicast reception with UDP; ICMP errors with
phase 2. Fragments are dropped and counted, never reassembled. IPv4 options are accepted on receive, ignored,
and never sent. ARP replies are not rate-limited, which matters once `accept_unsolicited_replies` is on: a
reply flood churns the cache. The ARP table stays Aloe's own fixed table; abseil does not enter the project.
