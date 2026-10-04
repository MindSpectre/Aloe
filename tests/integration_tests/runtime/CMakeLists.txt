##############################################################################
# The runtime over the ring driver: the echo per queue with real mbufs and registered threads
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Runtime.Ring
        test_runtime_ring.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Runtime.Ring
        PRIVATE
        Aloe::Common::Runtime
        Aloe::Common::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Runtime
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################
