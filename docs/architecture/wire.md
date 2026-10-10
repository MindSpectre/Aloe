# Wire Module

The wire module (`common/wire/`) is what Aloe knows about each protocol as values and formats: the
addresses, the header layouts, the protocol numbers and the checksums, grouped by protocol, with no packets,
no devices and no state. Everything about IPv4 sits under `wire/ipv4/`, everything about Ethernet under
`wire/ethernet/`, TCP under `wire/tcp/`, and IPv6 and UDP get a directory each when they arrive. It depends on nothing. Everything
is reached through the umbrella `#include <aloe/wire>` (`export/aloe/wire`), and targets link
`Aloe::Common::Wire`. The [device layer](device.md) speaks in its addresses; the [net](net.md) and [tcp](tcp.md)
bricks read and write its formats.

| Directory   | Target                  | Holds                                                                                    |
|-------------|-------------------------|------------------------------------------------------------------------------------------|
| `bytes/`, `checksum/` | `Aloe.Common.Wire.Bytes` | Big-endian loads and stores; the Internet checksum (RFC 1071).                 |
| `ethernet/` | `Aloe.Common.Wire.Ethernet` | `MacAddress`; `EthernetHeader`, `EtherType`.                                        |
| `ipv4/`     | `Aloe.Common.Wire.Ipv4` | `Ipv4Address` and its multicast MAC; `Ipv4Subnet`; `Ipv4Protocol`; `Ipv4Header`; the header checksum and the pseudo-header sum. |
| `arp/`      | `Aloe.Common.Wire.Arp`  | `ArpPacket`, `ArpOperation`, for Ethernet over IPv4.                                     |
| `icmp/`     | `Aloe.Common.Wire.Icmp` | `IcmpHeader`, `IcmpType`.                                                                |
| `tcp/`      | `Aloe.Common.Wire.Tcp`  | `TcpSequence`, `TcpFlag`, `TcpFlags`, `TcpHeader`, `TcpOptions`.                         |

## Key types

- **`aloe::wire::MacAddress`**, **`aloe::wire::Ipv4Address`** -- value types stored in transmission order,
  with parsing, formatting, and `load`/`store` over the bytes of a frame. An IPv4 address knows its special
  ranges (`is_unspecified`, `is_limited_broadcast`, `is_multicast`, `is_loopback`), and a group address maps
  to its Ethernet group address with `multicast_mac()` (RFC 1112).
- **`aloe::wire::Ipv4Subnet`** -- an address with its prefix: `mask()`, `contains()`, `network()`,
  `broadcast()`, and `has_broadcast()`, false for /31 and /32.
- **`aloe::wire::Ipv4Protocol`** -- the IPv4 protocol number as a type: `Icmp`, `Tcp`, `Udp` named, any other
  byte still representable.
- **The headers** -- `EthernetHeader`, `ArpPacket`, `Ipv4Header`, `IcmpHeader`, each with a static
  `parse(bytes)` returning `std::optional`, a const `write(out)`, and its length as `size`, all `constexpr`.
- **`aloe::wire::TcpSequence`**, **`TcpFlag`**/**`TcpFlags`**, **`TcpHeader`**, **`TcpOptions`** -- `TcpSequence`
  is a sequence number with serial arithmetic (`a - b` is the wrapped signed distance, `before`, `after`);
  `TcpFlags` is the flag byte as a set; `TcpHeader` keeps the data offset in bytes and leaves the options to
  `TcpOptions`, which reads the MSS only and reports a malformed list so the brick can drop the segment.
- **`aloe::wire::internet_checksum`** and friends -- the Internet checksum; in `ipv4/`, the IPv4 header
  checksum and the pseudo-header sum that transmit checksum offload starts from.
- **`aloe::wire::load_be16`**, **`store_be16`**, and the 32-bit pair -- network byte order over spans.

## Usage

```cpp
#include <aloe/wire>

std::array<std::byte, aloe::wire::EthernetHeader::size> frame{};
const aloe::wire::EthernetHeader sent{.destination = aloe::wire::MacAddress::broadcast(),
                                      .source      = aloe::wire::MacAddress{0x02, 0, 0, 0, 0, 1},
                                      .ethertype   = std::to_underlying(aloe::wire::EtherType::Arp)};
sent.write(frame);
const std::optional<aloe::wire::EthernetHeader> received = aloe::wire::EthernetHeader::parse(frame);  // == sent
```

## Design notes

A format is a type that reads and writes itself: `parse` is a static constructor that checks the bytes and
returns nothing for a frame it cannot accept, so a failed parse never leaves a half-filled object, and the
result can be a `const` local or checked in a `static_assert`. Headers `parse` and `write`, because parsing
can fail; addresses `load` and `store`, because copying a fixed number of bytes cannot.

The module is grouped by protocol rather than by kind, so one protocol is one directory: a reader looking
for "what does Aloe know about IPv4" opens `wire/ipv4/`, and the address, the header and the checksum are
side by side. Each protocol is a sub-library, so a target can link only the ones it reads.

It holds no state and no packets, which is why it sits below the device layer: a device reports its MAC
and steers by IPv4 addresses, and neither needs a packet type to say so. Every function is `constexpr`
and `noexcept`; a short buffer is an `assert` in a `store` or `load` and a `std::nullopt` from a `parse`.
