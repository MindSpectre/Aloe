/**
 * The Aloe wire module: addresses, headers and checksums, grouped by protocol, as values and formats with no packets
 * and no state. Consumers link Aloe::Common::Wire and write `#include <aloe/wire>`, which imports it.
 */
export module aloe.wire;

export import :arp;
export import :bytes;
export import :checksum;
export import :ethernet;
export import :icmp;
export import :ipv4_address;
export import :ipv4_checksum;
export import :ipv4_header;
export import :ipv4_protocol;
export import :ipv4_subnet;
export import :mac_address;
export import :tcp_header;
export import :tcp_options;
export import :tcp_sequence;
