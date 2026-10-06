# Fixtures

The `fixtures/` tier holds what Aloe ships for testing code written over the bricks, kept apart from the
stack in `common/` so that nothing there reads as something a production program links. A fixture
depends on `common/`; nothing in `common/` depends on a fixture. Targets are named `Aloe::Fixtures::<Module>`.

| Module   | Include                  | Target                   | Holds                                                                                       |
|----------|--------------------------|--------------------------|---------------------------------------------------------------------------------------------|
| `fabric` | `#include <aloe/fabric>` | `Aloe::Fixtures::Fabric` | The in-memory device backend: a broadcast domain of ports. See the [device layer](device.md). |
| `frames` | `#include <aloe/frames>` | `Aloe::Fixtures::Frames` | Frame builders and a parser over [`wire`](wire.md)'s formats, and packet byte helpers.       |

Aloe's own tests use both. Anyone testing a loop written over `Ipv4<Device>` or `ShardQueue<Device>` needs the
same two things: a device that runs with no root, no hugepages and no network card, and a way to make frames
and read the replies. The roadmap adds a scripted TCP peer here in phase 1. Helpers that only make sense
inside Aloe's own test binaries, such as the EAL as a gtest environment, stay in `tests/shared/`.

## Key types in `frames`

- **`aloe::frames::ethernet_frame`**, **`ipv4_frame`** with an **`Ipv4Spec`**, **`with_ipv4_options`** -- an
  Ethernet frame, or an IPv4 datagram with a UDP or TCP header, as bytes; `Checksums` picks correct, wrong or
  zero checksums. `pattern(length, seed)` is a recognisable payload.
- **`aloe::frames::arp_request`**, **`arp_reply`**, **`arp_frame`**, **`icmp_echo_frame`** with an **`EchoSpec`**
  -- the ARP and ICMP frames a peer would send.
- **`aloe::frames::parse_frame(bytes)`** -- a transmitted frame taken apart into a **`ParsedFrame`**: the
  Ethernet header, and the ARP packet, IPv4 header, ICMP header and L4 bytes where present. Nothing for a frame
  shorter than an Ethernet header. `l4_checksum_residue` checks a TCP or UDP checksum.
- **`aloe::frames::fill(packet, bytes)`**, **`bytes_of(packet)`** -- bytes into and out of a packet of any
  backend. `flow_of(spec)` gives the `device::FlowTuple` a card would hash.

## Usage

```cpp
#include <aloe/fabric>
#include <aloe/frames>

aloe::fabric::Fabric fabric;
aloe::fabric::Port& peer = fabric.add_port({.mac = {0x02, 0, 0, 0, 0, 2}});
// ... the code under test owns another port of the same fabric ...

auto packet = peer.allocate(0);
const auto request = aloe::frames::arp_frame(
    aloe::frames::arp_request({0x02, 0, 0, 0, 0, 2}, {10, 0, 0, 1}, {10, 0, 0, 2}),
    aloe::wire::MacAddress::broadcast());
std::ignore = aloe::frames::fill(*packet, request);
// transmit it, run the code under test, receive on `peer`, then:
// const auto reply = aloe::frames::parse_frame(aloe::frames::bytes_of(received));
```

## Design notes

The frames module has no gtest in it: `parse_frame` reports a short frame as `std::nullopt` instead of
failing an assertion, so a test of any framework can use it. Aloe's tests call `.value()`, which fails the
test with an exception when the frame is short.
