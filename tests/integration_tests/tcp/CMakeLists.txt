##############################################################################
# The TCP brick over the ring driver: a scripted peer on real mbufs, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        test_tcp_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Tcp.Ring
        PRIVATE
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
