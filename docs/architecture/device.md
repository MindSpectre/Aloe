# Device Layer

The device layer is the bottom of the stack: it moves Ethernet frames between the protocols above
and whatever carries them, and it reports what that carrier can do. It is three modules under
`component/`, so nothing above it links DPDK unless it chooses the DPDK backend.

| Module | Include | Target | Holds |
|---|---|---|---|
| `device` | `#include <aloe/device>` | `Aloe::Component::Device` | The `IsPacket` and `IsDevice` concepts, addresses, checksums and receive-side scaling. Header-only, no DPDK. |
| `fabric` | `#include <aloe/fabric>` | `Aloe::Component::Fabric` | The in-memory backend: a broadcast domain of ports for tests. No DPDK. |
| `ethdev` | `#include <aloe/ethdev>` | `Aloe::Component::Ethdev` | The DPDK backend: one port of any driver. The only module that links `Aloe::Dpdk`. |

The layer exists because network cards differ in what they can do. It hides everything about the
card; the shard runtime receives packets and a capability report, and nothing else that is
card-specific. Steering and thread concurrency are not the device layer's job; the runtime verifies
them on top of it.

## Key types

### Concepts, in `device`

- **`aloe::IsPacket`** -- a move-only owning handle over one contiguous buffer laid out as headroom,
  data, tailroom. `data()` is a byte span; `prepend` and `append` grow into the room and return the
  new bytes, or nothing when the room is too small; `trim_front` and `trim_back` shrink. `rx()`
  gives the `RxMetadata` the device filled on receive, `tx()` and `set_tx()` the `TxMetadata` the
  device reads on transmit. A default-constructed or moved-from packet is `empty()`. Every backend
  gives a fresh packet `aloe::packet_headroom`, 128 bytes, of headroom.
- **`aloe::IsDevice`** -- one port with N queues, queue i for shard i. `allocate(queue)`,
  `receive(queue, out)` and `transmit(queue, in)` are the hot path and never throw.
  `capabilities()` reports what the device can do, `steering()` the receive-side scaling in effect,
  `counters(queue)` what happened. `mac()`, `mtu()` and `link_up()` describe the port.
- **`aloe::Capabilities`** -- queue and MTU limits, whether the device hashes and steers, and which
  checksum offloads it verifies on receive or fills on transmit. Anything absent gets its software
  fallback in the stack above, never inside the device.
- **`aloe::RssDescription`**, **`aloe::FlowTuple`**, **`aloe::queue_for`** -- the steering rule.
  `queue_for(description, flow)` is a pure function of what the device reports, so the runtime can
  predict which queue any flow lands on. `aloe::toeplitz_hash` is the hash cards compute.
- **`aloe::MacAddress`**, **`aloe::Ipv4Address`** -- value types with parsing and formatting.
- **`aloe::internet_checksum`** and friends -- the Internet checksum, the IPv4 header checksum,
  and the IPv4 pseudo-header sum that transmit checksum offload starts from.

### Backends

- **`aloe::fabric::Fabric`** owns **`aloe::fabric::Port`**s on one broadcast domain. Unicast goes to
  the port with that MAC, the sender included; broadcast and multicast go to every other port. There
  is no loss, reordering or delay. A port with several queues steers by RSS exactly as a card would.
  A port opened with `EmulatedOffloads::Checksums` fills checksums on transmit and verifies them on
  receive, so both the offload path and the software path of the stack run in CI.
- **`aloe::ethdev::Eal`** starts DPDK once per process. **`aloe::ethdev::Port`** wraps one DPDK
  port: it configures the queues, the MTU, every checksum offload the driver has, and receive-side
  scaling under Aloe's key when the driver has that. A physical card, `net_tap`, `net_ring` and
  `net_null` are all just ports. A port claims its device through DPDK's ownership API, so a second
  `Port` on the same name fails without disturbing the first.

## Usage

Everything above the device layer is written against the concepts and instantiated with a
backend. This moves a frame through a fabric port and back:

```cpp
#include <aloe/fabric>

template <aloe::IsDevice Device>
std::size_t echo(Device& device, std::span<const std::byte> frame) {
    auto packet = device.allocate(0);
    if (!packet) {
        return 0;
    }
    std::ranges::copy(frame, packet->append(frame.size())->begin());
    std::array<typename Device::Packet, 1> burst{std::move(*packet)};
    std::ignore = device.transmit(0, burst);   // to the port with the destination MAC
    return device.receive(0, burst);           // 1 if the frame was addressed to this port
}

int main() {
    aloe::fabric::Fabric fabric;
    aloe::fabric::Port& port = fabric.add_port({.mac = aloe::MacAddress{0x02, 0, 0, 0, 0, 1}});
    // build a frame addressed to port.mac() ...
}
```

The same function runs unchanged over DPDK:

```cpp
#include <aloe/ethdev>

const std::vector<std::string> arguments = {"app", "--vdev=net_tap0,iface=aloe0"};
const aloe::ethdev::Eal eal{arguments};
aloe::ethdev::Port port{{.name = "net_tap0", .queues = 1}};
echo(port, frame);
```

Transmit checksum offload follows DPDK's convention on both backends: before asking the device to
fill the L4 checksum, write the IPv4 pseudo-header sum (`aloe::ipv4_pseudo_header_sum`) into the
checksum field and zero into the IPv4 checksum field, then set `TxMetadata` with the header lengths
and the fills wanted. Ask only for what `capabilities()` offers.

## Design notes

**One packet concept, one type per backend.** The fabric's packet is plain memory that ASan sees
every byte of; ethdev's packet is a handle over an `rte_mbuf` at zero overhead. Runtime and
protocol tests therefore contain no DPDK at all: they are small, start instantly, and ASan and TSan
see every byte and every handoff. The alternative, one mbuf-based packet everywhere with a DPDK
virtual device as the mock, was rejected because every test would start the EAL, which works once
per process, link 200 MB, and run with sanitizers blind inside DPDK, which is built without
instrumentation. The cost of the choice, two packet implementations, is paid by one conformance
suite (`tests/shared/device/device_conformance.hpp`) that runs against both backends.

**Steering is a pure function.** Inbound frames must reach the shard that owns the connection, and
outbound connections choose a local port so that the card's hash lands on the wanted shard. Both
backends therefore steer by the same rule, `queue_for`, over what the device reports is programmed:
a card may truncate the key or use its own table size, and `steering()` reflects the values read
back from it. Aloe's key starts with Microsoft's published default key, so the RSS verification
vectors apply, and an IPv4 4-tuple uses only the first 16 bytes of it, so a card that takes 40
bytes and one that takes 52 compute the same hash.

**The fabric is a test tool.** Delivery takes a lock per receive queue and copies frames twice, on
transmit and on receive, so a packet pool is only ever touched by its queue's thread. Simplicity
wins over speed. Scripted loss, reordering and delay arrive in a later phase; the delivery function
is the one place they plug in.

**Ethdev reports what is in effect.** A driver that cannot set the MTU keeps its own, one that
cannot update or read the indirection table is assumed to hold DPDK's default round-robin table,
and one that cannot report its key is assumed to hold Aloe's key truncated to its size. The ring
driver, which the CI tests use, has none of these; the null driver has all of them.

**Bursts.** A single `receive` or `transmit` moves at most `aloe::ethdev::Port::max_burst`, 64
packets, the size of the pointer array it hands to DPDK. `transmit` never accepts more than
offered and leaves every packet past its count untouched, so a caller retries or drops them.
A frame shorter than an Ethernet header or longer than `mtu() + 14` is refused on both backends:
`transmit` counts it as accepted, frees it and counts it as oversized on the transmitting queue.
