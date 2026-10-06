##############################################################################
# Smallest program that uses both dependencies: a sender pipeline and DPDK.
##############################################################################
add_executable(${EXAMPLES}.Hello
        hello.cpp
)
target_link_libraries(${EXAMPLES}.Hello
        PRIVATE
        Aloe::Common::Core
        Aloe::Common::Execution
        Aloe::Dpdk
)
##############################################################################
