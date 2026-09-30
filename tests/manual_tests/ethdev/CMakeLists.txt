##############################################################################
# Ethdev over a tap device against the kernel: needs CAP_NET_ADMIN, run by hand
##############################################################################
add_manual_test(${MANUAL_TESTING_TARGET}.Ethdev.Tap
        test_ethdev_tap.cpp
)
target_link_libraries(${MANUAL_TESTING_TARGET}.Ethdev.Tap
        PRIVATE
        Aloe::Component::Ethdev
        ${SHARED_TESTING_TARGET}.Device
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
