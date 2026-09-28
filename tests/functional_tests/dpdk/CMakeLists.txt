##############################################################################
# The guards in cmake/dpdk.cmake, exercised with made-up pkg-config data
##############################################################################
add_test(
        NAME ${FUNCTIONAL_TESTING_TARGET}.Dpdk.target_guards
        COMMAND ${CMAKE_COMMAND}
        -DDPDK_MODULE=${CMAKE_SOURCE_DIR}/cmake/dpdk.cmake
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/target_guards
        -P ${CMAKE_CURRENT_SOURCE_DIR}/dpdk_target_guards.cmake
)
set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Dpdk.target_guards PROPERTIES LABELS "functional")
##############################################################################
