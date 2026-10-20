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

    # The fixtures differ in p99 (+300 ns, +25 %) and max (+300 ns, +23 %): the default gates p50 to p99, --tails up
    # to max.
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "1 percentile(s) moved by more than 5.0%" ${FIXTURES}/baseline.json ${FIXTURES}/regressed.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression PROPERTIES
            LABELS "functional"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_the_tails_too
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "2 percentile(s) moved by more than 5.0%" ${FIXTURES}/baseline.json ${FIXTURES}/regressed.json --tails
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_the_tails_too PROPERTIES
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

    # p50 moves by one TSC step, 10 ns. At 1 % it would pass the default 5 % alone, so the threshold drops to 0.5 %
    # and the 20 ns floor is what passes it; with the floor at 0 the same step flags.
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.one_tsc_step_passes
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 0
            "no percentile moved" ${FIXTURES}/baseline.json ${FIXTURES}/one_step.json --threshold 0.5
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.one_tsc_step_passes PROPERTIES
            LABELS "functional"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.one_tsc_step_flags_without_the_floor
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "1 percentile(s) moved by more than 0.5% and 0 ns" ${FIXTURES}/baseline.json ${FIXTURES}/one_step.json
            --threshold 0.5 --min-ns 0
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.one_tsc_step_flags_without_the_floor
            PROPERTIES LABELS "functional"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_missing_row
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "(missing)" ${FIXTURES}/baseline.json ${FIXTURES}/missing.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_missing_row PROPERTIES
            LABELS "functional"
    )

    # A zero baseline has no percentage: its change prints as "n/a", marked when the candidate is above the floor.
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_zero_baseline
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 1
            "n/a!" ${FIXTURES}/zero.json ${FIXTURES}/baseline.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_zero_baseline PROPERTIES
            LABELS "functional"
    )

    # Same percentiles from another CPU, another build and another count: a warning, not a flag.
    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.warns_when_the_runs_differ
            COMMAND ${BASH_PROGRAM} ${SCENARIO} ${PYTHON3_PROGRAM} ${COMPARE} 0
            "warning: the runs differ in machine.cpu, run.build, the rows' count" ${FIXTURES}/baseline.json
            ${FIXTURES}/other_machine.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.warns_when_the_runs_differ PROPERTIES
            LABELS "functional"
    )
else ()
    message(STATUS "python3 or bash not found: the benchmark comparison tests are not registered")
endif ()
##############################################################################
