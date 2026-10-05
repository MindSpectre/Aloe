##############################################################################
# ARP and ping answered on one DPDK queue by a hand-written loop over the bricks: the net module's living example
##############################################################################
add_executable(${EXAMPLES}.PingResponder
        ping_responder.cpp
)
target_link_libraries(${EXAMPLES}.PingResponder
        PRIVATE
        Aloe::Common::Net
        Aloe::Common::Loop
        Aloe::Common::Ethdev
)
##############################################################################
