##############################################################################
# The scenario scripts: one gtest per script and checksum mode, discovered under tests/scenarios at run time
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Scenarios
        scenario_main.cpp
)
target_compile_definitions(${UNIT_TESTING_TARGET}.Scenarios
        PRIVATE
        ALOE_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/tests/scenarios"
)
target_link_libraries(${UNIT_TESTING_TARGET}.Scenarios
        PRIVATE
        Aloe::Fixtures::Scenario
        Aloe::Fixtures::Fabric
        GTest::gtest
        GTest::gmock
)
##############################################################################
