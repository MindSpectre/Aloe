##############################################################################
# The net brick over the ring driver: ARP, echo and send on real mbufs, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Net.Ring
        test_net_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Net.Ring
        PRIVATE
        Aloe::Common::Net
        Aloe::Common::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Net
        ${TEST_LIBS}
)
##############################################################################
