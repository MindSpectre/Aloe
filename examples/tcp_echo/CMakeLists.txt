##############################################################################
# TCP echo on one DPDK queue by a hand-written loop over the bricks: the tcp module's living example
##############################################################################
add_executable(${EXAMPLES}.TcpEcho
        tcp_echo.cpp
)
target_link_libraries(${EXAMPLES}.TcpEcho
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Loop
        Aloe::Common::Ethdev
)
##############################################################################
