set(TESTS_TARGET ${PROJECT_NAME}.Tests)
set(SHARED_TESTING_TARGET ${TESTS_TARGET}.Shared)
set(UNIT_TESTING_TARGET ${TESTS_TARGET}.Unit)
set(FUNCTIONAL_TESTING_TARGET ${TESTS_TARGET}.Functional)
set(INTEGRATION_TESTING_TARGET ${TESTS_TARGET}.Integration)
set(MANUAL_TESTING_TARGET ${TESTS_TARGET}.Manual)

set(TEST_LIBS
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
        GTest::gmock_main
)

add_subdirectory(shared)
add_subdirectory(unit_tests)
add_subdirectory(integration_tests)
add_subdirectory(functional_tests)
