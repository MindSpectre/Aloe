add_subdirectory(device)
add_subdirectory(fabric)
add_subdirectory(loop)
add_subdirectory(runtime)
add_subdirectory(net)

##############################################################################
# Test Core: execution facilities
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Core.Execution
        core/test_execution.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Core.Execution
        PRIVATE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Core: logging through the quill alias, into a file sink
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Core.Log
        core/test_log.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Core.Log
        PRIVATE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Core: version, and the umbrella header seen from two translation units
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Core.Version
        core/test_version.cpp
        core/test_umbrella_second_unit.cpp
)
target_compile_definitions(${UNIT_TESTING_TARGET}.Core.Version
        PRIVATE
        ALOE_EXPECTED_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(${UNIT_TESTING_TARGET}.Core.Version
        PRIVATE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Utils: discarding and choosing values, and the const and static guards
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Utils
        utils/test_guards.cpp
        utils/test_values.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Utils
        PRIVATE
        Aloe::Common::Utils
        ${TEST_LIBS}
)
##############################################################################
