##############################################################################
# The Ethernet echo on N shards of one DPDK port: the runtime's living example
##############################################################################
add_executable(${EXAMPLES}.EthernetEcho
        ethernet_echo.cpp
)
target_link_libraries(${EXAMPLES}.EthernetEcho
        PRIVATE
        Aloe::Common::Core
        Aloe::Common::Runtime
        Aloe::Common::Ethdev
)
##############################################################################
