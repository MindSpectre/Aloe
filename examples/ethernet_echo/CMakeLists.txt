##############################################################################
# The Ethernet echo on N shards of one DPDK port: the runtime's living example
##############################################################################
add_executable(${EXAMPLES}.EthernetEcho
        ethernet_echo.cpp
)
target_link_libraries(${EXAMPLES}.EthernetEcho
        PRIVATE
        Aloe::Common::Log
        Aloe::Common::Runtime
        Aloe::Common::Ethdev
        # Runtime and Ethdev are INTERFACE libraries that forward Device, Loop, Stream and Wire. CMake 4.4 gives a target
        # the plain BMIs of a module it reaches only through an INTERFACE library, beside the synthesized ones of the
        # modules it links directly, and clang cannot use the mix (ledger 10). Linked directly, they get synthesized
        # BMIs too.
        Aloe::Common::Device
        Aloe::Common::Loop
        Aloe::Common::Stream
        Aloe::Common::Wire
)
##############################################################################
