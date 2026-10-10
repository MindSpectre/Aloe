##############################################################################
# The kernel talks TCP to the brick and the runtime over a tap device: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Tcp.Tap
        test_tcp_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Tcp.Tap
        PRIVATE
        Aloe::Common::Runtime
        Aloe::Common::Tcp
        Aloe::Common::Net
        Aloe::Common::Ethdev
        ${SHARED_TESTING_TARGET}.Ethdev
        ${SHARED_TESTING_TARGET}.Log
        ${TEST_LIBS}
)
##############################################################################
