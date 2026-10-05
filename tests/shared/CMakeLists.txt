##############################################################################
# Helpers for tests that start the DPDK environment abstraction layer
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Dpdk INTERFACE
        dpdk/eal_arguments.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Dpdk INTERFACE
        dpdk/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Dpdk INTERFACE
        Aloe::Dpdk
)
##############################################################################

##############################################################################
# The device conformance suite every backend runs
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Device INTERFACE
        device/device_conformance.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Device INTERFACE
        device/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Device INTERFACE
        Aloe::Common::Device
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# The EAL as a gtest environment, and fresh virtual devices per test
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        dpdk/eal_environment.hpp
        dpdk/packet_socket.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        dpdk/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        ${SHARED_TESTING_TARGET}.Dpdk
        Aloe::Common::Ethdev
        Aloe::Common::Device
)
##############################################################################

##############################################################################
# Logging as a gtest environment
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Log INTERFACE
        log/logging_environment.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Log INTERFACE
        log/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Log INTERFACE
        Aloe::Common::Log
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# The Ethernet echo stack the runtime tests and examples run
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        runtime/echo_stack.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        runtime/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Runtime INTERFACE
        Aloe::Common::Runtime
        Aloe::Common::Loop
        Aloe::Common::Device
)
##############################################################################

##############################################################################
# The net brick over a fabric: the harness fixture
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Net INTERFACE
        net/net_fixture.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Net INTERFACE
        net/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Net INTERFACE
        Aloe::Common::Net
        Aloe::Fixtures::Fabric
        Aloe::Common::Loop
        ${SHARED_TESTING_TARGET}.Device
        ${TEST_LIBS}
)
##############################################################################
