/**
 * The Aloe fabric module: the in-memory device backend. Consumers link Aloe::Fixtures::Fabric and write
 * `#include <aloe/fabric>`, which imports it.
 */
export module aloe.fabric;

export import :fabric;
export import :fabric_packet;
export import :impairment;
export import :frame;
