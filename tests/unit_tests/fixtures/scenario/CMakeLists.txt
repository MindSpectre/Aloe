##############################################################################
# Test Scenario: the parser, the peer's numbers, the matcher, the harness and the runner
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Scenario
        test_parse_script.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Scenario
        PRIVATE
        Aloe::Fixtures::Scenario
        ${TEST_LIBS}
)
##############################################################################
