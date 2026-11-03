/**
 * The Aloe frames fixture module: builders for Ethernet, IPv4, ARP, ICMP and TCP frames as bytes, a parser that takes a
 * transmitted frame apart, a scripted TCP peer, and helpers that move bytes in and out of a packet of any backend.
 * Consumers link Aloe::Fixtures::Frames and write `#include <aloe/frames>`, which imports it.
 */
export module aloe.frames;

export import :frames;
export import :net_frames;
export import :tcp_frames;
export import :tcp_peer;
