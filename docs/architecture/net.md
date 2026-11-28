# Net Module

The net module (`common/net/`) is the first protocol brick: IPv4 over one device queue, with the ARP and
ICMP echo that serve it. One object per shard sits between the [loop](loop.md)'s `ShardQueue` and the
transports. It answers ARP and ping, sorts incoming IPv4 datagrams into one list per transport for the
layer above to drain, and sends a transport's segment by routing it, finding the next hop's MAC, from a static
table or, when the configuration switches learning on, from ARP, writing both headers and handing the frame to the
queue; for a transport that keeps its packets, it builds the headers
once and finishes and lends each frame built from them. Everything is reached through the umbrella
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
  returns a `SendError` and hands the packet back exactly as it was. For a transport that keeps its packets,
  `prebuild(destination, protocol, now)` returns the link and network headers to a destination as an
  `Ipv4Prebuilt`, and `send_frame(packet, request, now)` finishes a frame built from them, lends the packet to the
  device and transmits the loan, leaving the packet with the caller; `generation()` says when headers built
  earlier may name a stale next hop. `resolve(next_hop, now)`, `learn(address, mac, now)` and
  `apply_resolution(address, mac, now)` are the ways into the ARP cache from outside: the first asks, the second is
  the program's seed of the learned table, and the third takes a resolution another shard reported. `learn` answers
  what it did, an `ArpCache::Taught`: `Created` or `Updated` when it stored the mapping, and otherwise `Full` for a
  new address the learned table has no slot for, `Static` or `Conflict` for an address of the static table, and `Off`
  with learning off, where a neighbour is named in `static_arp` instead. Nothing here blocks, throws after
  construction, or allocates anything but packets after construction.
  **Transport access:** `Device` is the template parameter, `queue()` is the shard queue the brick was built
  over (its index, steering, ring and device), and `next_hop(destination)` answers the route question with no
  side effect: the destination on the subnet, the gateway off it, nothing with no gateway, and, with learning off,
  nothing for a hop outside the static table. `send` decides through the same call, and a transport asks it before
  committing resources, so a connect to an unroutable peer takes no slot. `resolves_neighbours()` says whether any
  unicast next hop can have a MAC: learning is on, or the static table holds an entry; a `tcp::Stack` refuses a brick
  that answers false. `Ipv4Config::report_unsolicited_replies`
  (off by default) lets the shard whose queue receives a reply that a sibling shard solicited report it in
  `resolved()` for forwarding, without storing it, since a multi-queue device's shards share one address; only a
  reply addressed to our address from a plausible sender is reported, and only with learning on.
- **`aloe::net::Ipv4Datagram<Packet>`** -- one received datagram for a transport: the whole frame as a packet
  the transport moves out to keep, the two addresses, the protocol, the header offsets, the L4 length from
  the IPv4 total length, and the device's L4 checksum verdict. `l4()` is the segment.
- **`aloe::net::Ipv4Config`** -- the address and prefix, an optional gateway, the TTL, the burst capacity
  the lists are sized to, the static ARP table, the switch for learning, the learned table's capacity and timings,
  and the switch for reporting replies a sibling shard solicited; see [Configuration](#configuration). The same
  config goes to every shard.
- **`aloe::net::SendRequest`**, **`SendError`** -- the destination, the protocol and which L4 checksum to
  fill; `NoRoute` (off the subnet with no gateway, or, with learning off, a next hop outside the static table),
  `Unresolved` (a request is on its way, or the learned table is full), `Oversized` or `Refused`.
- **`aloe::net::Ipv4Prebuilt`** -- the 34 bytes of an Ethernet and an IPv4 header to one destination, with the
  IPv4 total length, identification and checksum zero; `valid_until`, the stamp from which they should be
  built again; and `generation`, the brick's generation when they were built.
- **`aloe::net::ArpResolution`** -- an address and a MAC: an entry of the static table, and what `resolved()`
  reports for the loop to forward to the other shards.
- **`aloe::net::ArpCache`** -- the per-shard table of IPv4 address to MAC: the static entries, an array searched
  first and frozen for the cache's life, then, with learning on, a fixed open-addressing table of learned entries,
  aged lazily against the stamp the caller passes: reachable, then stale with a background refresh, then expired.
  `lookup` creates an entry for an absent address, `update` takes a frame's mapping and creates none, and `seed`
  takes the program's; `confirmed(address)` is a reachable learned entry's confirmation stamp, `is_static(address)`
  says whether an address is static, and `generation()` counts the changes to learned entries' MACs.
- **`aloe::net::Ipv4Counters`** -- what happened, one counter per drop reason and per send error, among them
  `dropped_arp_static_conflict`, an ARP frame that contradicted a static entry, and `arp_table_full`, a lookup or
  seed of a new address that found the learned table full.
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
aloe::loop::ShardQueueCounters counters;
aloe::loop::ShardQueue<aloe::ethdev::Port> queue{port, 0, 64, counters};
aloe::net::Ipv4<aloe::ethdev::Port> ip{queue, {.address      = {10, 77, 0, 2},
                                               .prefix       = 24,
                                               .gateway      = {{10, 77, 0, 1}},
                                               .arp_learning = true}};  // a tap interface's MAC is new on every run
std::vector<aloe::ethdev::Packet> burst(64);

aloe::core::calibrate_clock();
std::ignore = ip.resolve(*ip.gateway(), aloe::core::Clock::now());  // with learning on, ask for the router's MAC now
std::ignore = queue.flush();

while (running) {
    const auto now             = aloe::core::Clock::now();
    const std::size_t received = queue.receive(burst);
    ip.process(std::span{burst}.first(received), now);   // ARP and ping answered in here; nothing else to do yet
    std::ignore = queue.flush();
}
```

A deployment that knows its router names it in the static table instead and leaves learning off, the default: the
brick then sends no ARP request and learns from no frame, answers only requests for its own address, and a next hop
outside the table is `NoRoute`:

```cpp
aloe::net::Ipv4<aloe::ethdev::Port> ip{queue, {.address    = {192, 168, 1, 50},
                                               .prefix     = 24,
                                               .gateway    = {{192, 168, 1, 1}},
                                               .static_arp = {{.address = {192, 168, 1, 1}, .mac = router_mac}}}};
```

With transports the same loop gains one line per transport and nothing else. The transports are templated on
the IP type below them and transmit through it; TCP exists (see the [tcp page](tcp.md)), UDP comes after the
TCP, TLS and WebSocket path:

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

A transport that keeps its packets, as TCP keeps each segment until it is acknowledged, builds the headers
once and sends every frame built from them by reference:

```cpp
auto built = ip.prebuild(peer, aloe::wire::Ipv4Protocol::Tcp, now);
if (!built) {
    // NoRoute, or, with learning on, Unresolved (a request is out: retry later), as send fails.
}
// frame: a packet from allocate() holding built->headers, then the segment with its checksum field zero ...
const auto sent = ip.send_frame(frame, {.destination = peer, .protocol = aloe::wire::Ipv4Protocol::Tcp,
                                        .checksum = aloe::device::L4Checksum::Tcp}, now);
// Either way the packet is still yours: lent to the device on success, to be sent again once !frame.lent().
if (now >= built->valid_until || ip.generation() != built->generation) {
    built = ip.prebuild(peer, aloe::wire::Ipv4Protocol::Tcp, now);  // before the next frame
}
```

## Configuration

`Ipv4Config` is validated by the `Ipv4` constructor, which throws `std::invalid_argument` for a config it cannot run.
The learned table's four settings are read, and checked, only with learning on.

| Field                        | Default | Meaning                                                                                                         | Refused when                                                                                     |
|------------------------------|---------|-----------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `address`, `prefix`          | -, 24   | The brick's address and its subnet.                                                                             | the address is not a unicast host on its subnet; the prefix is above 32                          |
| `gateway`                    | none    | The next hop off the subnet; without one an off-subnet destination is `NoRoute`.                                | off the subnet, not another unicast host, or, with learning off, without a `static_arp` entry    |
| `ttl`                        | 64      | The TTL of every datagram sent.                                                                                 | zero                                                                                             |
| `burst_capacity`             | 64      | The most packets one `process` takes, which sizes the lists.                                                    | zero                                                                                             |
| `static_arp`                 | empty   | The static table: neighbours used without any ARP frame, and never changed.                                     | an address that is not another unicast host on the subnet, a zero or group MAC, an address twice |
| `arp_learning`               | false   | Resolve neighbours on the wire, and keep what is learned until it expires.                                      | never                                                                                            |
| `arp_capacity`               | 256     | The learned entries held at most.                                                                               | zero, or above 2^30                                                                              |
| `arp_reachable`              | 60 s    | A learned MAC is used this long after its confirmation with no refresh; after it, one refresh per interval.     | not positive, not below `arp_expire`, or above a quarter of `core::Duration`'s range             |
| `arp_expire`                 | 120 s   | A learned address is unresolved again this long after its confirmation.                                         | above a quarter of `core::Duration`'s range                                                      |
| `arp_request_interval`       | 1 s     | At most one request per entry per interval; an entry asked about and never answered gives its slot up after it. | not positive, or above a quarter of `core::Duration`'s range                                     |
| `report_unsolicited_replies` | false   | With learning on, report in `resolved()`, without storing it, a reply to us about an address not held.          | never                                                                                            |

No value of the timeouts disables the expiry: a learned entry always ages out. The upper bounds keep every stamp the
table computes from an entry's confirmation from overflowing.

## Design notes

**IPv4 in a folder of its own.** The brick, its four parts and the ARP cache live under `ipv4/`, and a type that
is IPv4's alone says so in its name: `Ipv4Datagram`, `Ipv4Prebuilt`, `Ipv4Config`, `Ipv4Counters`. `SendRequest`
and `SendError` are what a transport is written against and carry no prefix. An IPv6 brick would sit beside
`ipv4/` in the same module.

**Four parts behind one façade.** `net::Ipv4` is the brick's public face and the owner of its config and counters; what it does is four classes in `net::detail`, each in its own header under `ipv4/stack/detail/`: `Routing` (the subnet, the gateway and the broadcast rules, as route questions with no side effect), `Arp` (the cache, the requests, the replies and the `resolved()` list; it reaches the queue alone), `Output` (`send`, `prebuild`, `send_frame`, the headers and checksums in one place and the identification counter; it routes through `Routing` and resolves through `Arp`) and `Input` (the frame sorted by ethertype, the IPv4 checks, delivery into one list per transport, the echo reply built in its own packet and framed by `Output`, as `send` frames a datagram once the hop is known). The dependency runs one way, input to output to ARP, read off the includes. The counters are one struct every part writes through a reference, so a reader of `Ipv4Counters` sees the whole brick.

**The seam is a list, not a call.** IPv4 cannot be templated on who reads it, so it does not call anyone.
It sorts the datagrams it accepts into one list per transport, and the loop hands each list to its brick.
A core that runs TCP alone writes one line and pays nothing for the UDP list; a core that runs a feed and a
gateway writes two, in the order it wants. The dispatch is explicit code the program owns, which is what
the bricks promise, and the runtime writes those lines once.

**ARP is private to IP.** Nothing above the brick sees ARP, as nothing above a socket does. What leaks is the route
question and, with learning on, what startup and the multi-shard case need. With learning off `next_hop` answers
nowhere for a neighbour outside the static table, so `send` and `prebuild` fail `NoRoute` at once and a transport
takes no slot. With learning on `send` and `prebuild` fail `Unresolved` while a request is out or the learned table is
full, and the caller retries, which TCP's ARP poll and retransmission timer absorb; `resolved()` reports every mapping
a frame gave an entry the brick holds; `learn` seeds the learned table and reports nothing, so shards never echo each
other. ARP frames carry no IP header, so a card puts them on its default queue, queue 0 on both backends; the loop that
owns that queue forwards what its brick reports to the other shards. A request from another host for a third party
updates an entry we already hold and never creates one, so a router's gratuitous ARP after a failover takes effect at
once; a sender that cannot be a host (a zero, multicast or broadcast address) is answered when it asks for our address
and never learned, which keeps RFC 5227 probes working. A request from a group MAC is dropped as martian instead,
since the reply would go to a group.

**Neighbours come from two places: a static table, and learning that is off by default.** `Ipv4Config::static_arp`
lists address and MAC pairs. The table is built once, in the constructor, and frozen for the life of the brick: no
frame, no call and no timer adds to it, changes it or removes from it. It is searched first, before anything learned,
and it is an array: a handful of addresses are compared in a row faster than any table is hashed, the more so since
the order path makes no lookup at all, each connection keeping its link header built. Learning is what the brick does
with `arp_learning` on: asking on the wire, keeping the answer and aging it. It is off by default, because the first
deployment is a trading system that knows its next hop, and a learned entry always expires, since a table that only
gains addresses can be filled by whoever supplies them.

With learning off, the default:

- No ARP request is ever sent, and no frame teaches anything.
- A request for our own address is answered, to the frame's sender MAC: the router needs ours.
- `next_hop` answers nowhere for a destination whose next hop has no static entry, so `send` and `prebuild` fail
  `NoRoute` at once; TCP's `connect` returns `NoRoute` with no slot taken, and a SYN from such a neighbour is dropped
  unanswered. Nothing polls and nothing waits, and since the static table cannot change, a connection that opened
  never meets `NoRoute` afterwards.
- The constructor refuses a gateway with no static entry: a start that fails instead of every connect failing.
- A brick with no static entry at all is still built, since an echo responder or a program that only receives needs
  no neighbour; it answers ARP and echo requests at the frame's source MAC, and `resolves_neighbours()` is false. A
  `tcp::Stack` over it is refused at construction, for the same reason as the gateway.
- `learn` stores nothing and answers `Off`, and `resolve` answers from the static table alone.

For a static address, in both modes:

- A lookup answers with its MAC and asks for no request, at any stamp, and `prebuild` gives headers whose `valid_until`
  is the end of time, as for a broadcast destination. The static table never moves the generation, and with learning
  off nothing does, so a template is built once per connection.
- An ARP frame whose sender address is static and whose sender MAC is the configured one teaches nothing and is
  reported nowhere. One whose sender MAC is another is counted in `dropped_arp_static_conflict`, and nothing else
  about it changes. Either way a request for our own address is still answered.

The operating rule: a static entry does not follow a MAC that changes, so it is pointed at the router's virtual MAC
(VRRP or HSRP), which survives a failover, and the conflict counter is what shows a router whose MAC changed.

**With learning on, the learned table follows three rules.**

1. *An entry changes only through a frame about its own address.* A reply, or a request, whose sender is an address
   the table holds fills that entry in or gives it another MAC. It extends the entry's life only when it answers a
   request the brick itself has outstanding for that address, the incomplete entry's or a stale entry's refresh: a
   frame nobody asked for never moves the confirmation stamp, so an entry the brick does not use expires `arp_expire`
   after the last answer to its own request, whatever frames say. A frame about an address the table does not hold
   changes nothing and creates no entry. Only the brick's own lookup, which a send, a `prebuild` or a `resolve` makes,
   and the program's `learn` create one. The cost: the first segment to a host on the subnet that asked for our address
   waits for an ARP exchange of our own, where a host that learns from requests already has its MAC.
2. *No address ever takes another's place.* Any free slot serves any address, and a resolved entry leaves the table
   only by expiring. While `arp_capacity` entries are held, a new address may take the slot of an incomplete entry
   whose last request is at least `arp_request_interval` old, or failing that of an expired entry; with neither it is
   refused: the lookup fails `Unresolved`, no request leaves, and `arp_table_full` counts it. Nothing is evicted.
3. *A slot that never resolved is given up.* An entry that was asked about and never answered is not a mapping: once
   its last request is an `arp_request_interval` old, another address may take its slot, so sends to addresses that
   do not exist cannot hold the table for the two minutes an expiry takes.

What learning gives and what it does not. The table is of a fixed size and only the brick's own sends fill it, so no
frame makes it grow or pushes a neighbour out. But a forged frame about an address the table holds is learned, as on
any host that uses ARP; and whoever can make the brick send to addresses on the subnet that do not exist (a SYN from a
forged source to a listener, any segment to a closed port) can keep its free slots busy with unresolved entries, so
that a new neighbour waits, and can make the brick refresh a stale entry it holds, which a forged frame then answers.
Resolved entries are not touched either way. A deployment that cannot accept that runs with the default.

**Aging without a timer.** The learned table keeps the stamp of each entry's last confirmation and compares it with
the stamp the caller passes. Fresh entries are used as they are; stale ones, `arp_reachable` after their confirmation,
are still used while one unicast refresh goes out per interval, so a live flow never stalls on a refresh; expired ones,
`arp_expire` after it, are unresolved again. No timer node, no wheel, and the brick's constructor takes the queue and
the config and nothing else. The table is open addressing with linear probing and backward-shift deletion, as TCP's
flow table, over the next power of two at or above twice `arp_capacity` slots, so it is never more than half full;
with learning off it is not allocated.

**The echo reply never consults the cache.** It is built in the request's packet and goes back to the
frame's source MAC: the last hop, which is the right next hop back whether the pinger is on the subnet or
behind a router. That is what makes `ping` work on any shard without cross-shard ARP.

**A template knows when its next hop may have changed.** `prebuild` routes and looks the next hop up as `send`
does, sends the ARP request when the cache asks for one, fails with `NoRoute` or `Unresolved` as `send` does,
and says how long its headers hold: `valid_until` is the entry's confirmation plus `arp_reachable` while the
entry is fresh, one `arp_request_interval` from now while it is stale and wants a refresh, and never for a
broadcast or multicast destination or a static neighbour. `generation()` is the cache's count of changes to a
learned MAC: it moves by one when a frame, a forwarded resolution or a `learn` fills an incomplete entry or gives an
entry another MAC, when a lookup finds a reachable entry expired, and when a new address takes the slot of an expired
one. The brick keeps one generation, so with learning on a change to any learned neighbour makes every template build
again on its next send, a static hop's too; a generation per hop would change TCP's template and what it asks of the
brick. A holder builds again when the stamp reaches `valid_until` or the generation moved, two compares per frame, and
the rebuild is the lookup `send` would have run, so a live flow keeps its entry refreshed and follows a MAC
that changed. `send_frame` takes the finished frame by reference: it writes the total length, a fresh
identification and the IPv4 checksum, fills the L4 checksum or writes the pseudo-header sum and requests the
offload, sets the transmit metadata, lends the packet and transmits the loan, so the packet stays with the
caller for a resend once `lent()` is false. A refused transmit drops the loan, puts the per-datagram fields and
the L4 checksum field back to zero and returns `Refused` with the frame as the caller built it; a frame longer
than the MTU is `Oversized`. `datagrams_sent` and `send_refused` count as they do for `send`, which stays for
ARP, ICMP and every caller that does not keep its packet.

**Checksums in one place.** `send`, `send_frame` and the echo reply share one function: the IPv4 header
checksum and, when asked, the L4 checksum, filled by the device when `capabilities()` offers it and in
software when not. A transport never computes a checksum and never learns the offload convention. The fabric
emulates the offload, so both paths run in CI.

**Every slot empty.** `process` moves a packet into a list, transmits it as a reply, or releases it, so a
hand-written loop resets nothing and the runtime's leftover sweep finds nothing. Datagrams whose source is a
multicast, limited-broadcast or loopback address, or our own address, are dropped and counted as martian, as
the kernel does at IP input; TCP never sees them. Broadcasts reach UDP, and never TCP: RFC 1122 makes TCP
unicast only, so a TCP segment to the limited or subnet broadcast is dropped and counted here rather than
left to every transport to check.

**A config that cannot be a host is refused.** The address must be a unicast host on its subnet: not zero,
multicast, loopback or 255.255.255.255, and not the subnet's network or broadcast address when the prefix is
30 or less (a /31 or /32 has neither). The gateway must be another such host on the same subnet. A static entry's
address must be another such host too, listed once, at a MAC that is neither zero nor a group address; and with
learning off the gateway must have a static entry.

**Resolutions cross shards through the loop.** On a multi-queue device every shard has the same address, so with
learning on the gateway's reply to a request shard 2 sent arrives on queue 0, whose brick may never have asked. Every
shard's brick on such a device sets `report_unsolicited_replies`. `resolved()` then reports every mapping a frame gave
an entry the shard holds, a refresh with the same MAC included, which carries a reply to the sibling that asked when
queue 0 holds the address too, as it does for a gateway every shard uses; and also a reply addressed to us about an
address the shard does not hold, which is not stored. The loop that owns queue 0 hands each reported resolution to
every other shard, which calls `apply_resolution(address, mac, now)` on its own thread: it fills or updates an entry
that shard holds, extending its life only when it answers that shard's own request, and otherwise does nothing. It
reports nothing, so a forwarded resolution is never forwarded back, and a reply is used only by a shard that asked.
With learning off nothing is reported and nothing is forwarded. The runtime's `TcpStack` does all of this (see the
[runtime page](runtime.md)); it sets the flag itself when its queue's device has more than one queue. A hand-written
multi-queue program sets the flag in the `Ipv4Config` it gives every shard and owns the channel: one `loop::Inbox` per
loop, and a `loop::Work` node carrying the address and the MAC, pushed to every other loop's inbox after each `process`
that reported something and run there by a call to `apply_resolution`. Each `resolved()` list is forwarded once, after
the `process` that filled it, never again on a pass that received nothing. A forward costs one node per sibling per
reported frame, so a flood of replies is a flood of forwards.

**Where ARP departs from the RFCs and from Linux**, each with who decided it:

- **Nothing is resolved on the wire by default.** A host as RFC 1122 describes it resolves its neighbours with ARP;
  the brick does so only when its configuration switches learning on. By the author's decision.
- **A request for our address teaches nothing about a sender the table does not hold**, where RFC 826's algorithm adds
  the sender when we are the target and Linux's `arp_process` creates the entry at once (`neigh_event_ns`): a frame that
  can create an entry can fill the table. By the author's decision, on a proposal made when the brick was hardened.
- **A full learned table refuses a new address and evicts nothing**, where Linux collects old entries to make room
  (`neigh_forced_gc`). By the author's decision.
- **A frame nobody asked for does not extend a learned entry's life**, which is Linux's rule too, since `arp_process`
  leaves such an entry stale; but the brick's test is whether it asked, not which frame answered, so it departs from
  Linux both ways: a unicast reply nobody asked for confirms nothing here, where Linux takes it as reachable, and while
  a request is outstanding a request or a broadcast reply confirms, which Linux leaves stale. A choice made when the
  brick was hardened, after a security check of the build.
- **A frame that contradicts a static entry is counted**, in `dropped_arp_static_conflict`; Linux leaves a permanent
  entry alone and counts nothing. As issue #23 asks.

Not built, where Linux has it: `locktime`, which keeps a valid entry's MAC for a second against a frame that names
another. It stops two hosts flapping a learned entry; it does not stop a forger, who sends again a second later. The
static table is the protection.

**Deferred.** Flow rules come with later placement work; multicast reception with UDP; ICMP errors with
phase 2. Fragments are dropped and counted, never reassembled. IPv4 options are accepted on receive, ignored,
and never sent. Nothing bounds the forwards between shards under a flood of replies, with learning on over several
queues. Nothing notices that a static neighbour died or changed its MAC: nothing probes a static entry, and the
conflict counter and the connections' own timeouts are what an operator sees. The ARP table stays Aloe's own fixed
table; abseil does not enter the project.
