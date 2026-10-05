# Wire Module

The wire module (`common/wire/`) is what Aloe knows about each protocol as values and formats: the
addresses, the header layouts, the protocol numbers and the checksums, grouped by protocol, with no packets,
no devices and no state. Everything about IPv4 sits under `wire/ipv4/`, everything about Ethernet under
`wire/ethernet/`, and IPv6, UDP and TCP get a directory each when they arrive. It depends on nothing. Everything
is reached through the umbrella `#include <aloe/wire>` (`export/aloe/wire`), and targets link
`Aloe::Common::Wire`. The [device layer](device.md) speaks in its addresses; the [net](net.md) bricks read
and write its formats.

| Directory   | Target                  | Holds                                                                                    |
|-------------|-------------------------|------------------------------------------------------------------------------------------|
| `bytes/`, `checksum/` | `Aloe.Common.Wire.Bytes` | Big-endian loads and stores; the Internet checksum (RFC 1071).                 |
| `ethernet/` | `Aloe.Common.Wire.Ethernet` | `MacAddress`; `EthernetHeader`, `EtherType`, `ethernet_header_size`.                 |
| `ipv4/`     | `Aloe.Common.Wire.Ipv4` | `Ipv4Address` and its multicast MAC; `Ipv4Protocol`; `Ipv4Header`; the header checksum and the pseudo-header sum. |
| `arp/`      | `Aloe.Common.Wire.Arp`  | `ArpPacket`, `ArpOperation`, for Ethernet over IPv4.                                     |
| `icmp/`     | `Aloe.Common.Wire.Icmp` | `IcmpHeader`, `IcmpType`.                                                                |

## Key types

- **`aloe::wire::MacAddress`**, **`aloe::wire::Ipv4Address`** -- value types stored in transmission order,
  with parsing, formatting, and `load`/`store` over the bytes of a frame. `multicast_mac(group)` maps an
  IPv4 group address to its Ethernet group address (RFC 1112).
- **`aloe::wire::Ipv4Protocol`** -- the IPv4 protocol number as a type: `Icmp`, `Tcp`, `Udp` named, any other
  byte still representable.
- **The headers** -- `EthernetHeader`, `ArpPacket`, `Ipv4Header`, `IcmpHeader`, each with a `parse_*`
  returning `std::optional` and a `write_*` over byte spans, all `constexpr`.
- **`aloe::wire::internet_checksum`** and friends -- the Internet checksum; in `ipv4/`, the IPv4 header
  checksum and the pseudo-header sum that transmit checksum offload starts from.
- **`aloe::wire::load_be16`**, **`store_be16`**, and the 32-bit pair -- network byte order over spans.

## Usage

```cpp
#include <aloe/wire>

std::array<std::byte, aloe::wire::ethernet_header_size> frame{};
aloe::wire::write_ethernet(frame, {.destination = aloe::wire::MacAddress::broadcast(),
                                   .source      = aloe::wire::MacAddress{0x02, 0, 0, 0, 0, 1},
                                   .ethertype   = std::to_underlying(aloe::wire::EtherType::Arp)});
const auto header = aloe::wire::parse_ethernet(frame);  // the same three fields back
```

## Design notes

The module is grouped by protocol rather than by kind, so one protocol is one directory: a reader looking
for "what does Aloe know about IPv4" opens `wire/ipv4/`, and the address, the header and the checksum are
side by side. Each protocol is a sub-library, so a target can link only the ones it reads.

It holds no state and no packets, which is why it sits below the device layer: a device reports its MAC
and steers by IPv4 addresses, and neither needs a packet type to say so. Every function is `constexpr`
and `noexcept`; a short buffer is an `assert` in a `store` or `load` and a `std::nullopt` from a `parse`.
