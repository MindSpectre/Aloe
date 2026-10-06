#include <aloe/fabric>
#include <aloe/wire>

#include <device_conformance.hpp>
#include <gtest/gtest.h>

namespace aloe::testing {

    namespace {

        /// One single-queue fabric port. A frame to its own MAC comes straight back.
        struct FabricFixture {
            fabric::Fabric fabric;
            fabric::Port& port = fabric.add_port(single_queue_port());

            static fabric::PortConfig single_queue_port() {
                return {
                    .mac = wire::MacAddress{0x02, 0, 0, 0, 0, 0x01},
                      .queues = 1, .pool_size = 64
                };
            }

            fabric::Port& device() {
                return port;
            }
        };

    }  // namespace

    INSTANTIATE_TYPED_TEST_SUITE_P(Fabric, DeviceConformance, FabricFixture);

}  // namespace aloe::testing
