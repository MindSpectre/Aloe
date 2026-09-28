##############################################################################
# DPDK starts without privileges and sees its statically linked drivers
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Dpdk.EalSmoke
        test_eal_smoke.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Dpdk.EalSmoke
        PRIVATE
        ${SHARED_TESTING_TARGET}.Dpdk
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Two DPDK processes at once do not disturb each other
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Dpdk.EalConcurrent
        test_eal_concurrent.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Dpdk.EalConcurrent
        PRIVATE
        ${SHARED_TESTING_TARGET}.Dpdk
        ${TEST_LIBS}
)
##############################################################################
