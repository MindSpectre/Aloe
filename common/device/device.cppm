/**
 * The Aloe device module: the Packet and Device concepts, the packet metadata and receive-side scaling. The addresses
 * and checksums a device speaks in come from the wire module. Consumers link Aloe::Common::Device and write
 * `#include <aloe/device>`, which imports it.
 */
export module aloe.device;

export import :device;
export import :packet;
export import :rss;
