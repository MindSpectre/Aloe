##############################################################################
# Test Scenario: the parser, the peer's numbers, the matcher, the harness and the runner
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Scenario
        test_parse_script.cpp
        test_peer.cpp
        test_notation.cpp
        test_match.cpp
        test_harness.cpp
        test_runner.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Scenario
        PRIVATE
        Aloe::Fixtures::Scenario
        Aloe::Fixtures::Frames
        ${TEST_LIBS}
)
##############################################################################
