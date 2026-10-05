##############################################################################
# The kernel pings the brick over a tap device: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Net.Tap
        test_net_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Net.Tap
        PRIVATE
        Aloe::Common::Net
        Aloe::Common::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
