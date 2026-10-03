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
# Frame builders and packet helpers shared by the device backend tests
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Device INTERFACE
        device/frames.hpp
        device/device_conformance.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Device INTERFACE
        device/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Device INTERFACE
        Aloe::Component::Device
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# The EAL as a gtest environment, and fresh virtual devices per test
##############################################################################
add_library(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        dpdk/eal_environment.hpp
)
target_include_directories(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        dpdk/
)
target_link_libraries(${SHARED_TESTING_TARGET}.Ethdev INTERFACE
        ${SHARED_TESTING_TARGET}.Dpdk
        Aloe::Component::Ethdev
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
        Aloe::Common::Core
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
        Aloe::Component::Runtime
        Aloe::Component::Device
)
##############################################################################
