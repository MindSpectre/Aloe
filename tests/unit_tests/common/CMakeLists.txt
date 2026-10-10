add_subdirectory(wire)
add_subdirectory(device)
add_subdirectory(loop)
add_subdirectory(runtime)
add_subdirectory(net)
add_subdirectory(stream)
add_subdirectory(tcp)

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
# Test Core: discarding and choosing values, and the const and static guards
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Core.Helpers
        core/test_guards.cpp
        core/test_values.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Core.Helpers
        PRIVATE
        Aloe::Common::Core
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Execution: the facilities alias
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Execution
        execution/test_execution.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Execution
        PRIVATE
        Aloe::Common::Execution
        ${TEST_LIBS}
)
##############################################################################

##############################################################################
# Test Log: logging through the quill alias, into a file sink
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Log
        log/test_log.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Log
        PRIVATE
        Aloe::Common::Log
        ${TEST_LIBS}
)
##############################################################################
