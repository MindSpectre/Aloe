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
