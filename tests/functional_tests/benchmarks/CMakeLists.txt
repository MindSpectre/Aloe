##############################################################################
# scripts/compare-benchmark.py against fixture result files; skipped without python3
##############################################################################
find_program(PYTHON3_PROGRAM python3)
if (PYTHON3_PROGRAM)
    set(COMPARE ${CMAKE_SOURCE_DIR}/scripts/compare-benchmark.py)
    set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression
            COMMAND ${PYTHON3_PROGRAM} -I ${COMPARE} ${FIXTURES}/baseline.json ${FIXTURES}/regressed.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression PROPERTIES
            LABELS "functional"
            PASS_REGULAR_EXPRESSION "2 percentile\\(s\\) moved by more than 5.0%"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes
            COMMAND ${PYTHON3_PROGRAM} -I ${COMPARE} ${FIXTURES}/baseline.json ${FIXTURES}/baseline.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes PROPERTIES
            LABELS "functional"
            PASS_REGULAR_EXPRESSION "no percentile moved"
    )
else ()
    message(STATUS "python3 not found: the benchmark comparison tests are not registered")
endif ()
##############################################################################
