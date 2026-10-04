##############################################################################
# The runtime echo over a tap device against the kernel: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Runtime.Tap
        test_runtime_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Runtime.Tap
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
