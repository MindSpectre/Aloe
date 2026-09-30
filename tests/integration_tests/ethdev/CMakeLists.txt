##############################################################################
# Ethdev over the ring driver: the conformance suite, queues and back-pressure
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Ethdev.Ring
        test_ethdev_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Ethdev.Ring
        PRIVATE
        Aloe::Component::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Ethdev over the null driver: capabilities, steering, the packet and the EAL
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Ethdev.Null
        test_ethdev_eal.cpp
        test_ethdev_null.cpp
        test_ethdev_packet.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Ethdev.Null
        PRIVATE
        Aloe::Component::Ethdev
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
