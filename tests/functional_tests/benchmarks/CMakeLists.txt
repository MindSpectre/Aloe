##############################################################################
# scripts/compare-benchmark.py against fixture result files, through a wrapper that checks the exit status and the
# message; skipped without python3 or bash
##############################################################################
find_program(PYTHON3_PROGRAM python3)
find_program(BASH_PROGRAM bash)
if (PYTHON3_PROGRAM AND BASH_PROGRAM)
    set(COMPARE ${CMAKE_SOURCE_DIR}/scripts/compare-benchmark.py)
    set(SCENARIO ${CMAKE_CURRENT_SOURCE_DIR}/compare_scenario.sh)
    set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "2 percentile(s) moved by more than 5.0%" ${FIXTURES}/baseline.json ${FIXTURES}/regressed.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression PROPERTIES
            LABELS "functional"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 0
            "no percentile moved" ${FIXTURES}/baseline.json ${FIXTURES}/baseline.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes PROPERTIES
            LABELS "functional"
    )
else ()
    message(STATUS "python3 or bash not found: the benchmark comparison tests are not registered")
endif ()
##############################################################################
